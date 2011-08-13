#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <omphalos/pcap.h>
#include <linux/wireless.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <omphalos/hwaddrs.h>
#include <omphalos/netaddrs.h>
#include <omphalos/wireless.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

static pthread_t *input_tid;

// Call whenever we generate output, so that the prompt is updated
static inline void
wake_input_thread(void){
	if(input_tid){
		pthread_kill(*input_tid,SIGWINCH);
		rl_redisplay(); // FIXME probably need call from readline context
	}
}

// FIXME this ought return a string rather than printing it
#define IFF_FLAG(flags,f) ((flags) & (IFF_##f) ? #f" " : "")
static int
print_iface(FILE *fp,const interface *iface){
	const char *at;
	int n = 0;

	if((at = lookup_arptype(iface->arptype,NULL)) == NULL){
		fprintf(stderr,"Unknown dev type %u\n",iface->arptype);
		return -1;
	}
	n = fprintf(fp,"[%8s][%s] %d %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
		iface->name,at,iface->mtu,
		IFF_FLAG(iface->flags,UP),
		IFF_FLAG(iface->flags,BROADCAST),
		IFF_FLAG(iface->flags,DEBUG),
		IFF_FLAG(iface->flags,LOOPBACK),
		IFF_FLAG(iface->flags,POINTOPOINT),
		IFF_FLAG(iface->flags,NOTRAILERS),
		IFF_FLAG(iface->flags,RUNNING),
		IFF_FLAG(iface->flags,PROMISC),
		IFF_FLAG(iface->flags,ALLMULTI),
		IFF_FLAG(iface->flags,MASTER),
		IFF_FLAG(iface->flags,SLAVE),
		IFF_FLAG(iface->flags,MULTICAST),
		IFF_FLAG(iface->flags,PORTSEL),
		IFF_FLAG(iface->flags,AUTOMEDIA),
		IFF_FLAG(iface->flags,DYNAMIC),
		IFF_FLAG(iface->flags,LOWER_UP),
		IFF_FLAG(iface->flags,DORMANT),
		IFF_FLAG(iface->flags,ECHO)
		);
	if(n < 0){
		return -1;
	}
	if(!(iface->flags & IFF_LOOPBACK)){
		int nn;

		nn = fprintf(fp,"\t   driver: %s %s @ %s\n",iface->drv.driver,
				iface->drv.version,iface->drv.bus_info);
		if(nn < 0){
			return -1;
		}
		n += nn;
	}
	return n;
}
#undef IFF_FLAG

static int
print_stats(FILE *fp){
	interface total;

	memset(&total,0,sizeof(total));
	if(printf("<stats>") < 0){
		return -1;
	}
	if(print_all_iface_stats(fp,&total) < 0){
		return -1;
	}
	if(print_pcap_stats(fp,&total) < 0){
		return -1;
	}
	if(print_iface_stats(fp,&total,NULL,"total") < 0){
		return -1;
	}
	if(printf("</stats>") < 0){
		return -1;
	}
	return 0;
}

static int
dump_output(FILE *fp){
	if(fprintf(fp,"<omphalos>") < 0){
		return -1;
	}
	if(print_stats(fp)){
		return -1;
	}
	if(fprintf(fp,"</omphalos>\n") < 0 || fflush(fp)){
		return -1;
	}
	return 0;
}

static int
print_neigh(const interface *iface,const struct l2host *l2){
	char *hwaddr;
	int n;

	hwaddr = l2addrstr(l2);
	n = printf("[%8s] neighbor %s\n",iface->name,hwaddr);
	free(hwaddr);
	/* FIXME printf("[%8s] neighbor %s %s%s%s%s%s%s%s%s\n",iface->name,str,
			nd->ndm_state & NUD_INCOMPLETE ? "INCOMPLETE" : "",
			nd->ndm_state & NUD_REACHABLE ? "REACHABLE" : "",
			nd->ndm_state & NUD_STALE ? "STALE" : "",
			nd->ndm_state & NUD_DELAY ? "DELAY" : "",
			nd->ndm_state & NUD_PROBE ? "PROBE" : "",
			nd->ndm_state & NUD_FAILED ? "FAILED" : "",
			nd->ndm_state & NUD_NOARP ? "NOARP" : "",
			nd->ndm_state & NUD_PERMANENT ? "PERMANENT" : ""
			);
		*/
	return n;
}

static int
print_host(const interface *iface,const struct l2host *l2,const struct l3host *l3){
	char *hwaddr,*netaddr;
	const char *l3name;
	int n;

	hwaddr = l2addrstr(l2);
	netaddr = l3addrstr(l3);
	if( (l3name = get_l3name(l3)) ){
		n = printf("[%8s] host %s \"%s\" (addr %s)\n",iface->name,hwaddr,l3name,netaddr);
	}else{
		n = 0;
	//	n = printf("[%8s] host %s addr %s\n",iface->name,hwaddr,netaddr);
	}
	free(netaddr);
	free(hwaddr);
	return n;
}

static int
print_wireless_event(FILE *fp,const interface *i,unsigned cmd){
	int n = 0;

	switch(cmd){
	case SIOCGIWSCAN:{
		// FIXME handle scan results
		n = fprintf(fp,"\t   Scan results on %s\n",i->name);
	break;}case SIOCGIWAP:{
		// FIXME handle AP results
		n = fprintf(fp,"\t   Access point on %s\n",i->name);
	break;}case IWEVASSOCRESPIE:{
		// FIXME handle IE reassociation results
		n = fprintf(fp,"\t   Reassociation on %s\n",i->name);
	break;}default:{
		n = fprintf(fp,"\t   Unknown wireless event on %s: 0x%x\n",i->name,cmd);
		break;
	} }
	return n;
}

static void
packet_cb(omphalos_packet *op){
	// We won't have l2s/l2d on critically malformed frames, or UI-driving
	// frames (clock ticks, interface events, etc).
	if(op->l2s && op->l2d){
		const char *ns = NULL,*nd = NULL;

		//ns = get_name(op->l2s);
		ns = ns ? ns : get_devname(op->l2s);
		//nd = get_name(op->l2d);
		nd = nd ? nd : get_devname(op->l2d);
		//printf("[%s] %s -> %s %04hx\n",op->i->name,ns,nd,op->l3proto);
	}
}

static inline void
clear_for_output(FILE *fp){
	fprintf(fp,"\r");
}

#define PROMPTDELIM "> "
static pthread_mutex_t promptlock = PTHREAD_MUTEX_INITIALIZER;
static char promptbuf[IFNAMSIZ + __builtin_strlen(PROMPTDELIM) + 1] = "> ";

static void *
iface_event(interface *i,void *unsafe __attribute__ ((unused))){
	pthread_mutex_lock(&promptlock);
	snprintf(promptbuf,sizeof(promptbuf),"%s"PROMPTDELIM,i->name);
	pthread_mutex_unlock(&promptlock);
	clear_for_output(stdout);
	print_iface(stdout,i);
	rl_set_prompt(promptbuf);
	wake_input_thread();
	return NULL;
}
#undef PROMPTDELIM

static void *
neigh_event(const struct interface *i,struct l2host *l2){
	clear_for_output(stdout);
	print_neigh(i,l2);
	wake_input_thread();
	return NULL;
}

static void *
host_event(const struct interface *i,struct l2host *l2,struct l3host *l3){
	clear_for_output(stdout);
	print_host(i,l2,l3);
	wake_input_thread();
	return NULL;
}

static void *
wireless_event(interface *i,unsigned cmd,void *unsafe __attribute__ ((unused))){
	clear_for_output(stdout);
	print_wireless_event(stdout,i,cmd);
	wake_input_thread();
	return NULL;
}

static int cancelled;

static void
handle_quit(void){
	cancelled = 1;
}

static void
handle_dev(void){
	// FIXME
}

// FIXME need be able to pass the handlers arguments!
static void *
tty_handler(void *v){
	const struct {
		const char *cmd;
		void (*fxn)(void);
		const char *help;
	} cmdtable[] = {
		{ .cmd = "dev",		.fxn = handle_dev,	.help = "select an interface",	},
		{ .cmd = "quit",	.fxn = handle_quit,	.help = "exit the program",	},
		{ .cmd = NULL,		.fxn = NULL,		.help = NULL, }
	};

	while(!v && !cancelled){
		char *l = readline(promptbuf); // FIXME need to use promptlock!

		if(l == NULL){
			break;
		}
		if(*l){
			const typeof(*cmdtable) *c;

			add_history(l);
			for(c = cmdtable ; c->cmd ; ++c){
				if(strcmp(c->cmd,l) == 0){
					c->fxn();
					break;
				}
			}
#define HELPSTR "help"
#define HELP(cmd,help) printf("%s\t%s\n",cmd,help)
			if(c->cmd == NULL){
				if(strcmp(l,HELPSTR) == 0){
					for(c = cmdtable ; c->cmd ; ++c){
						HELP(c->cmd,c->help);
					}
					HELP(HELPSTR,"this list");
				}else{
					fprintf(stderr,"Unknown command: '%s'. Use '"HELPSTR"' for help.\n",l);
				}
#undef HELP
#undef HELPSTR
			}
		}
		free(l);
	}
	printf("Shutting down...\n");
	/*kill(getpid(),SIGINT);
	pthread_exit(NULL);*/
	exit(0); // FIXME
}

static int
init_tty_ui(pthread_t *tid){
	int err;

	rl_prep_terminal(1); // 1 == read eight-bit input
	if( (err = pthread_create(tid,NULL,tty_handler,NULL)) ){
		fprintf(stderr,"Couldn't launch input thread (%s?)\n",strerror(err));
		return -1;
	}
	return 0;
}

static void
cleanup_tty_ui(void){
	rl_deprep_terminal();
}

int main(int argc,char * const *argv){
	omphalos_ctx pctx;
	pthread_t tid;

	if(omphalos_setup(argc,argv,&pctx)){
		return EXIT_FAILURE;
	}
	pctx.iface.iface_event = iface_event;
	pctx.iface.neigh_event = neigh_event;
	pctx.iface.host_event = host_event;
	pctx.iface.wireless_event = wireless_event;
	pctx.iface.packet_read = packet_cb;
	if(!pctx.pcapfn){ // FIXME, ought be able to use UI with pcaps
		input_tid = &tid;
		if(init_tty_ui(input_tid)){
			omphalos_cleanup(&pctx);
			return EXIT_FAILURE;
		}
	}
	if(omphalos_init(&pctx)){
		cleanup_tty_ui();
		return EXIT_FAILURE;
	}
	cleanup_tty_ui();
	if(dump_output(stdout) < 0){
		if(errno != ENOMEM){
			fprintf(stderr,"Couldn't write output (%s?)\n",strerror(errno));
		}
		return EXIT_FAILURE;
	}
	omphalos_cleanup(&pctx);
	return EXIT_SUCCESS;
}

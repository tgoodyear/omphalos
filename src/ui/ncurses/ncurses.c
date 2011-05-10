#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>
#include <signal.h>
#include <ncurses.h>
#include <pthread.h>
#include <sys/utsname.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

#define PROGNAME "omphalos"	// FIXME
#define VERSION  "0.98pre"	// FIXME

enum {
	BORDER_COLOR = 1,
	HEADING_COLOR = 2,
};

static pthread_t inputtid;
static struct utsname sysuts;

// FIXME do stuff here, proof of concept skeleton currently
static void *
ncurses_input_thread(void *nil){
	int ch;

	if(!nil){
		while((ch = getch()) != 'q' && ch != 'Q');
		printf("DONE\n");
		raise(SIGINT);
	}
	pthread_exit(NULL);
}

static int
draw_main_window(WINDOW *w,const char *name,const char *ver){
	if(wcolor_set(w,BORDER_COLOR,NULL) != OK){
		return -1;
	}
	if(box(w,0,0) != OK){
		return -1;
	}
	if(mvwprintw(w,0,2,"[") < 0){
		return -1;
	}
	if(wattron(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		return -1;
	}
	if(wprintw(w,"%s %s on %s %s",name,ver,sysuts.sysname,sysuts.release) < 0){
		return -1;
	}
	if(wattroff(w,A_BOLD | COLOR_PAIR(HEADING_COLOR)) != OK){
		return -1;
	}
	if(wcolor_set(w,BORDER_COLOR,NULL) != OK){
		return -1;
	}
	if(wprintw(w,"]") < 0){
		return -1;
	}
	if(wrefresh(w)){
		return -1;
	}
	if(wcolor_set(w,0,NULL) != OK){
		return -1;
	}
	if(pthread_create(&inputtid,NULL,ncurses_input_thread,NULL)){
		return -1;
	}
	return 0;
}

// Cleanup which ought be performed even if we had a failure elsewhere, or
// indeed never started.
static int
mandatory_cleanup(WINDOW *w){
	int ret = 0;

	if(delwin(w) != OK){
		ret = -1;
	}
	if(endwin() != OK){
		ret = -1;
	}
	if(ret){
		fprintf(stderr,"Couldn't cleanup ncurses\n");
	}
	return ret;
}

static WINDOW *
ncurses_setup(void){
	WINDOW *w;

	if((w = initscr()) == NULL){
		fprintf(stderr,"Couldn't initialize ncurses\n");
		goto err;
	}
	if(cbreak() != OK){
		fprintf(stderr,"Couldn't disable input buffering\n");
		goto err;
	}
	if(noecho() != OK){
		fprintf(stderr,"Couldn't disable input echoing\n");
		goto err;
	}
	if(start_color() != OK){
		fprintf(stderr,"Couldn't initialize ncurses color\n");
		goto err;
	}
	if(use_default_colors()){
		fprintf(stderr,"Couldn't initialize ncurses colordefs\n");
		goto err;
	}
	if(init_pair(BORDER_COLOR,COLOR_GREEN,COLOR_BLACK) != OK){
		fprintf(stderr,"Couldn't initialize ncurses colorpair\n");
		goto err;
	}
	if(init_pair(HEADING_COLOR,COLOR_YELLOW,COLOR_BLACK) != OK){
		fprintf(stderr,"Couldn't initialize ncurses colorpair\n");
		goto err;
	}
	if(curs_set(0) == ERR){
		fprintf(stderr,"Couldn't disable cursor\n");
		goto err;
	}
	if(draw_main_window(w,PROGNAME,VERSION)){
		fprintf(stderr,"Couldn't use ncurses\n");
		goto err;
	}
	return w;

err:
	mandatory_cleanup(w);
	return NULL;
}

// Bind one of these state structures to each interface
typedef struct iface_state {
	int scrline;
	uintmax_t pkts;
} iface_state;

static int
print_iface_state(const interface *i,const iface_state *is){
	return mvprintw(is->scrline,2,"[%8s] %ju",i->name,is->pkts);
}

static void
packet_callback(const interface *i,void *unsafe){
	iface_state *is = unsafe;

	if(unsafe){
		++is->pkts;
		print_iface_state(i,is);
		refresh();
	}
}

static void *
interface_callback(const interface *i,void *unsafe){
	static uintmax_t events = 0; // FIXME
	static unsigned ifaces = 0; // FIXME
	iface_state *ret;

	if((ret = unsafe) == NULL){
		if( (ret = malloc(sizeof(iface_state))) ){
			++ifaces;
			ret->scrline = 3 + ifaces;
			ret->pkts = 0;
			print_iface_state(i,ret);
		}
	}
	mvprintw(3,2,"events: %ju (most recent on %s)",++events,i->name);
	refresh();
	return ret;
}

int main(int argc,char * const *argv){
	omphalos_ctx pctx;
	WINDOW *w;

	if(setlocale(LC_ALL,"") == NULL){
		fprintf(stderr,"Couldn't initialize locale (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if(uname(&sysuts)){
		fprintf(stderr,"Coudln't get OS info (%s?)\n",strerror(errno));
		return EXIT_FAILURE;
	}
	if((w = ncurses_setup()) == NULL){
		return EXIT_FAILURE;
	}
	if(omphalos_setup(argc,argv,&pctx)){
		return EXIT_FAILURE;
	}
	pctx.iface.packet_read = packet_callback;
	pctx.iface.iface_event = interface_callback;
	if(omphalos_init(&pctx)){
		goto err;
	}
	omphalos_cleanup();
	if(mandatory_cleanup(w)){
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;

err:
	mandatory_cleanup(w);
	return EXIT_FAILURE;
}

#include <errno.h>
#include <iwlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <omphalos/diag.h>
#include <linux/wireless.h>
#include <omphalos/wireless.h>
#include <omphalos/omphalos.h>
#include <omphalos/interface.h>

static inline int
get_wireless_extension(const char *name,int cmd,struct iwreq *req){
	int fd;

	if(strlen(name) >= sizeof(req->ifr_name)){
		diagnostic("Name too long: %s",name);
		return -1;
	}
	if((fd = socket(AF_INET,SOCK_DGRAM,0)) < 0){
		diagnostic("Couldn't get a socket (%s?)",strerror(errno));
		return -1;
	}
	strcpy(req->ifr_name,name);
	if(ioctl(fd,cmd,req)){
		//diagnostic("ioctl() failed (%s?)",strerror(errno));
		close(fd);
		return -1;
	}
	if(close(fd)){
		diagnostic("Couldn't close socket (%s?)",strerror(errno));
		return -1;
	}
	return 0;
}

static int
wireless_rate_info(const char *name,wless_info *wi){
	const struct iw_param *ip;
	struct iwreq req;

	if(get_wireless_extension(name,SIOCGIWRATE,&req)){
		return -1;
	}
	ip = &req.u.bitrate;
	wi->bitrate = ip->value;
	return 0;
}

static int
wireless_freq_info(const char *name,wless_info *wi){
	struct iw_range range;
	int fd;

	assert(wi);
	if((fd = socket(AF_INET,SOCK_DGRAM,0)) < 0){
		diagnostic("Couldn't get a socket (%s?)",strerror(errno));
		return -1;
	}
	if(iw_get_range_info(fd,name,&range)){
		diagnostic("Couldn't get range info on %s (%s)",name,strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

int handle_wireless_event(const omphalos_iface *octx,interface *i,
				const struct iw_event *iw,size_t len){
	if(len < IW_EV_LCP_LEN){
		diagnostic("Wireless msg too short on %s (%zu)",i->name,len);
		return -1;
	}
	switch(iw->cmd){
	case SIOCGIWSCAN:{
		// FIXME handle scan results
	break;}case SIOCGIWAP:{
		// FIXME handle AP results
	break;}case SIOCGIWSPY:{
		// FIXME handle AP results
	break;}case SIOCSIWMODE:{
		// FIXME handle wireless mode change
	break;}case SIOCSIWFREQ:{
		// FIXME handle frequency/channel change
	break;}case IWEVASSOCRESPIE:{
		// FIXME handle IE reassociation results
	break;}case SIOCSIWESSID:{
		// FIXME handle ESSID change
	break;}case SIOCSIWRATE:{
		// FIXME doesn't this come as part of the netlink message? this
		// is an extra 3 system calls...
		wireless_rate_info(i->name,&i->settings.wext);
	break;}case SIOCSIWTXPOW:{
		// FIXME handle TX power change
	break;}default:{
		diagnostic("Unknown wireless event on %s: 0x%x",i->name,iw->cmd);
		return -1;
	} }
	if(octx->wireless_event){
		i->opaque = octx->wireless_event(i,iw->cmd,i->opaque);
	}
	return 0;
}

static inline uintmax_t
iwfreq_defreak(const struct iw_freq *iwf){
	uintmax_t ret = iwf->m;
	unsigned e = iwf->e;

	while(e--){
		ret *= 10;
	}
	return ret;
}

int iface_wireless_info(const char *name,wless_info *wi){
	struct iwreq req;

	memset(wi,0,sizeof(*wi));
	memset(&req,0,sizeof(req));
	if(get_wireless_extension(name,SIOCGIWNAME,&req)){
		return -1;
	}
	if(wireless_rate_info(name,wi)){
		wi->bitrate = 0; // no bitrate for eg monitor mode
	}
	if(wireless_freq_info(name,wi)){
		return -1;
	}
	if(get_wireless_extension(name,SIOCGIWMODE,&req)){
		return -1;
	}
	wi->mode = req.u.mode;
	if(get_wireless_extension(name,SIOCGIWFREQ,&req)){
		wi->freq = 0; // no frequency for eg unassociated managed mode
	}else{
		wi->freq = iwfreq_defreak(&req.u.freq);
	}
	return 0;
}

#define FREQ_80211A	0x01
#define FREQ_80211B	0x02
#define FREQ_80211G	0x04
#define FREQ_80211N	0x08
#define FREQ_80211Y	0x10
#define FREQ_24		(FREQ_80211B|FREQ_80211G|FREQ_80211N)
static const struct freq {
	unsigned hz;		// unique Hz
	unsigned channel;	// channel #. multiple freqs per channel!
	unsigned modes;		// bitmask of FREQ_* values
} freqtable[] = {
	{ 2412,		1,	FREQ_24,	},
	{ 2417,		2,	FREQ_24,	},
	{ 2422,		3,	FREQ_24,	},
	{ 2427,		4,	FREQ_24,	},
	{ 2432,		5,	FREQ_24,	},
	{ 2437,		6,	FREQ_24,	},
	{ 2442,		7,	FREQ_24,	},
	{ 2447,		8,	FREQ_24,	},
	{ 2452,		9,	FREQ_24,	},
	{ 2457,		10,	FREQ_24,	},
	{ 2462,		11,	FREQ_24,	},
	{ 2467,		12,	FREQ_24,	},
	{ 2472,		13,	FREQ_24,	},
	{ 2482,		14,	FREQ_24,	},
};

unsigned wireless_freq_count(void){
	return sizeof(freqtable) / sizeof(*freqtable);
}

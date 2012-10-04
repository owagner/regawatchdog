/*

  Homematic CCU ReGaHSS Watchdog
  Written by Oliver Wagner <owagner@vapor.com>
  
  How this works:

  We monitor whether the system variable WATCHDOG was modified within the last
  3 minutes. If not, we kill the last generated ReGaHSS forked process.
  
  If we can't determine the variable timestamp after 60 minutes,
  or if there is no ReGaHSS instance, we reboot.
  
  Additionally, we're using the system watchdog /dev/watchdog.

*/

#define VERSION "0.12"
                 
#define _XOPEN_SOURCE /* glibc2 needs this */
       
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <syslog.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

int not_seen_count=0;
int disabled;

int killtime=3;
int initwaittime=20;
int reboottime=60;
int loglevel=LOG_DEBUG;
int disable_watchdog;
int watchdog_fd=-1;

void l(int pri,const char *s,...)
{
  va_list ap;
  
  if(pri>loglevel)
  	  return;
  
  va_start(ap,s);

  vsyslog(pri,s,ap);
  vprintf(s,ap);
  putchar('\n');
}

void switcher(int sig)
{
  if(disabled)
  {
    l(LOG_INFO,"regawatchdog: enabled");
    disabled=0;
  }
  else
  {
    l(LOG_INFO,"regawatchdog: disabled");
    disabled=1;
  }
}

void closewatchdog(int sig)
{
  if(watchdog_fd>=0)
  {
    write(watchdog_fd,"V",1);
    close(watchdog_fd);
  }
  exit(1);
}

void reboot(void)
{
  system("/usr/local/addons/regawatchdog/before_reboot");
  system("/sbin/reboot");
}

void dokill(void)
{
  DIR *proc;
  struct dirent *d;
  unsigned long long oldest_j=0;
  pid_t oldest_pid=0;

  // We need to scan /proc here
  proc=opendir("/proc");
  while((d=readdir(proc)))
  {
    char pathname[256];
    FILE *f;
    
    snprintf(pathname,sizeof(pathname),"/proc/%s/stat",d->d_name);
    f=fopen(pathname,"r");
    if(f)
    {
      pid_t pid;
      unsigned long long j;
      char name[32];
      
      if(fscanf(f,"%d %32s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu",&pid,name,&j)==3)
      {
        if(!strcmp(name,"(ReGaHss)"))
        {
          // We found a ReGaHSS instance
          if(j>oldest_j)
          {
            oldest_j=j;
            oldest_pid=pid;
          }
        }
      }

      fclose(f);     
    }
  
  }
  closedir(proc);  

  if(oldest_pid>1)
  {
    l(LOG_CRIT,"Found newest ReGaHSS instance at pid %d (jiffies %llu), sending SIGTERM",oldest_pid,oldest_j);  
    system("/usr/local/addons/regawatchdog/before_kill");
    kill(oldest_pid,SIGTERM);  
  }
  else
  {
    l(LOG_CRIT,"No ReGaHSS instance found, rebooting");
    reboot();
  }
}
 
void loadcfg(void)
{
	FILE *f=fopen("/usr/local/addons/regawatchdog/regawatchdog.cfg","r");
	char buffer[128];
	
	if(!f)
		return;
	while(fgets(buffer,sizeof(buffer),f))
	{
		char *key=strtok(buffer,"=");
		char *val=strtok(NULL,"=");
		if(key && val)
		{
			if(!strcasecmp(key,"REBOOTTIME"))
			{
				reboottime=atoi(val);
			}
			else if(!strcasecmp(key,"KILLTIME"))
			{
				killtime=atoi(val);
			}
			else if(!strcasecmp(key,"INITWAITTIME"))
			{
				initwaittime=atoi(val);
			}
			else if(!strcasecmp(key,"LOGLEVEL"))
			{
				loglevel=atoi(val);
			}
			else if(!strcasecmp(key,"NOSYSTEMWATCHDOG"))
			{
				disable_watchdog=atoi(val);
			}
			else
			{
				l(LOG_ERR,"Unknown config key %s",key);
				exit(1);
			}
		}
	}
	fclose(f);
}

char *querycmd="tclsh <<\"EOF\"\n\
load tclrega.so\n\
array set r [rega_script \"var v=dom.GetObject('WATCHDOG');WriteLine(v.Timestamp());\"]\n\
puts $r(STDOUT)\n\
EOF\n";


void dowatch(void)
{
  FILE *cmd=popen(querycmd,"r");
  char buffer[256],*res;
  struct tm tm;
  time_t now;
  int age;
  
  fgets(buffer,sizeof(buffer),cmd);
  while(fgetc(cmd)>=0);
  pclose(cmd);

  time(&now);
  tm=*localtime(&now);
  res=strptime(buffer,"%Y-%m-%d %T\n",&tm);
  if(!res || *res)
  {
    if(not_seen_count++>=reboottime)
    {
      l(LOG_CRIT,"Unable to determine WATCHDOG timestamp for more than %d minutes, rebooting",not_seen_count);
      reboot();
      exit(0);
    }
    l(LOG_DEBUG,"Unable to determine WATCHDOG timestamp for %d minutes",not_seen_count);
    return;
  }
  // Ok, file exists.
  not_seen_count=0;
  age=(now-mktime(&tm))/60;
  l(LOG_DEBUG,"Last WATCHDOG update was %i minutes ago (%.19s)",age,buffer);
  if(initwaittime)
  {
    l(LOG_DEBUG,"Will wait for %d more minutes for startup to complete",initwaittime);
    initwaittime--;
  }
  else if(age>=killtime)
    dokill();
}

int main(int argc,char **argv)
{
  signal(SIGHUP,switcher);
  signal(SIGINT,closewatchdog);
  signal(SIGTERM,closewatchdog);
  openlog("regawatchdog",0,LOG_DAEMON);
  loadcfg();
  l(LOG_INFO,"Starting V" VERSION ": killtime=%dm, initwaittime=%dm, reboottime=%dm, loglevel=%d, system_watchdog=%d",killtime,initwaittime,reboottime,loglevel,!disable_watchdog);
  if(!disable_watchdog)
  {
    int wdt;
    watchdog_fd=open("/dev/watchdog",O_WRONLY);
    ioctl(watchdog_fd,WDIOC_GETTIMEOUT,&wdt);
    l(LOG_INFO,"System watchdog timeout was %ds, setting to 256s",wdt);
    wdt=256;
    ioctl(watchdog_fd,WDIOC_SETTIMEOUT,&wdt);
  }
  for(;;)
  {
    if(watchdog_fd>=0)
    {
      if(write(watchdog_fd,"",1)!=1)
      {
        l(LOG_CRIT,"Unable to write to /dev/watchdog?!?");
      }
    }
    sleep(60);
    if(!disabled)
      dowatch();
  }
}


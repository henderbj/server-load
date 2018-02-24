#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include "../inih-r41/ini.h"

typedef struct
{
  const char* rpc_auth;
  long netspeed;
  const char* iface;
  int cpus;
} configuration;

typedef struct
{
  unsigned long long int total;
  unsigned long long int idle;
} cpu_sample;

typedef struct
{
  unsigned long long int rx_bytes;
  unsigned long long int tx_bytes;
} net_sample;

// This structure is used by main to communicate with parse_opt.
struct arguments
{
  char *args[2];            /* ARG1 and ARG2 */
  int verbose;              /* The -v flag */
  char *outfile;            /* Argument for -o */
  char *string1, *string2;  /* Arguments for -a and -b */
};

// OPTIONS.  Field 1 in ARGP.
// Order of fields: {NAME, KEY, ARG, FLAGS, DOC}.
static struct argp_option options[] =
{
  {"verbose", 'v', 0, 0, "Produce verbose output"},
  {"output",  'o', "OUTFILE", 0,
   "Output to OUTFILE instead of to standard output"},
  {0}
};


static int handler(void* server, const char* section, const char* name, const char* value)
{
  configuration* pconfig = (configuration*)server;

  #define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
  if (MATCH("server_load", "RPC_AUTH")) {
    pconfig->rpc_auth = strdup(value);
  } else if (MATCH("server_load", "NETSPEED")) {
    pconfig->netspeed = atol(value);
  } else if (MATCH("server_load", "IFACE")) {
    pconfig->iface = strdup(value);
  } else if (MATCH("server_load", "CPUS")) {
    pconfig->cpus = atoi(value);
  } else {
    return 0;  /* unknown section/name, error */
  }
  return 1;
}

unsigned long long int array_sum(FILE *log_fp, unsigned long long int a[])
{
  unsigned long long int total=0;
  for (int i=0; i<10; i++)
  {
    total+=a[i];
  }
  return total;
}

int update_tag(FILE* log_fp, const char* rpc_auth, char* tag, char* value)
{
  char* serf="/usr/local/bin/serf";
  char cmd[512];
  int result;
  sprintf(cmd, "%s tags -rpc-auth=%s -set %s=%s", serf, rpc_auth, tag, value);
  fprintf(log_fp, "Setting serf tag %s=%s\n", tag, value);
  result=system(cmd);
  return result;
}

void take_cpu_sample(unsigned long long int p[])
{
  FILE *fp=NULL;
  fp = fopen("/proc/stat", "r");
  fscanf(fp, "%*s");
  for (int i=0; i<10; i++)
    fscanf(fp, "%Ld", &p[i]);
  fclose(fp);
  return;
}

unsigned long long int read_net_from_file(char* filename)
{
  FILE *fp=NULL;
  unsigned long long int value;
  fp = fopen(filename, "r");
  fscanf(fp, "%Ld", &value);
  fclose(fp);
  return value;
}

net_sample take_net_sample(const char* iface)
{
  char file[512];
  net_sample sample;
  sprintf(file, "/sys/class/net/%s/statistics/rx_bytes", iface);
  sample.rx_bytes=read_net_from_file(file);
  sprintf(file, "/sys/class/net/%s/statistics/tx_bytes", iface);
  sample.tx_bytes=read_net_from_file(file);
  return sample;
}

int main(int argc, char* argv[])
{
  FILE *log_fp=NULL;
  pid_t process_id = 0;
  pid_t sid = 0;
  char period = 10; //delay in seconds between samples
  unsigned long long int cpu[10];
  cpu_sample cpu_actual={.total=0, .idle=0}, cpu_old={.total=0, .idle=0}, cpu_delta={.total=0, .idle=0};
  net_sample net_actual={.rx_bytes=0, .tx_bytes=0}, net_old={.rx_bytes=0, .tx_bytes=0}, net_delta={.rx_bytes=0, .tx_bytes=0};
  int loadavg=0, rxload=0, txload=0;
  int tagcpu=-100, tagrx=-100, tagtx=-100, threshold=5;
  char tagvalue[10];
  configuration config;
  time_t rawtime;
  struct tm *info_tm;
  char time_buffer[80];

  process_id = fork();

  // Indication of fork() failure
  if (process_id < 0)
  {
    printf("fork failed!\n");
    // Return failure in exit status
    exit(1);
  }
  // PARENT PROCESS. Need to kill it.
  if (process_id > 0)
  {
    printf("process_id of child process %d \n", process_id);
    // return success in exit status
    exit(0);
  }
  //unmask the file mode
  umask(0);
  //set new session
  sid = setsid();
  if(sid < 0)
  {
    // Return failure
    exit(1);
  }
  // Change the current working directory to root.
  chdir("/");
  // Close stdin. stdout and stderr
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  // Open a log file in write mode.
  log_fp = fopen ("/var/log/load.log", "a");
  fprintf(log_fp, "%s: Starting background process...\n", argv[0]);

  if (ini_parse("/root/scripts/vpn/.env.ini", handler, &config) < 0)
  {
    fprintf(log_fp, "Can't load '/root/scripts/vpn/.env.ini'\n");
    return 1;
  }
//  fprintf(log_fp, "Config loaded from '.env.ini': RPC_AUTH=%s, NETSPEED=%ld, CPUS=%d\n",
//    config.rpc_auth, config.netspeed, config.cpus);

  while (1)
  {
    fflush(log_fp);
    // Implement and call some function that does core work for this background process.
    time( &rawtime );
    info_tm = localtime( &rawtime );
    strftime(time_buffer,80,"%x-%X", info_tm);
    take_cpu_sample(cpu);
    cpu_actual.total=array_sum(log_fp,cpu);
    cpu_actual.idle=cpu[3];
    if ( cpu_old.total )
    {
      cpu_delta.total = cpu_actual.total - cpu_old.total;
      cpu_delta.idle = cpu_actual.idle - cpu_old.idle;
      loadavg=100*(cpu_delta.total-cpu_delta.idle)/cpu_delta.total;
      if ( abs(loadavg-tagcpu) > threshold )
      {
        //Set new tag value
        sprintf(tagvalue, "%d", loadavg);
        update_tag(log_fp, config.rpc_auth, "cpu", tagvalue);
        tagcpu=loadavg;
      }
    }
    net_actual = take_net_sample(config.iface);
    if ( net_old.rx_bytes )
    {
      net_delta.rx_bytes = net_actual.rx_bytes - net_old.rx_bytes;
      //1024*1024/8=131072, which converts netspeed to bytes/s maximum
      rxload = 100*(net_delta.rx_bytes/period)/(config.netspeed*131072);
      if ( abs(rxload-tagrx) > threshold )
      { 
        //Set new tag value
        sprintf(tagvalue, "%d", rxload);
        update_tag(log_fp, config.rpc_auth, "rx", tagvalue);
        tagrx=rxload;
      }
      net_delta.tx_bytes = net_actual.tx_bytes - net_old.tx_bytes;
      txload = 100*(net_delta.tx_bytes/period)/(config.netspeed*131072);
      if ( abs(txload-tagtx) > threshold )
      {
        //Set new tag value
        sprintf(tagvalue, "%d", txload);
        update_tag(log_fp, config.rpc_auth, "tx", tagvalue);
        tagtx=txload;
      }
    }
    fprintf(log_fp, "%s: dcputotal=%Lu, dcpuidle=%Lu, cpu=%d%%, drx_bytes=%Ld, rxload=%d%%, dtx_bytes=%Ld, txload=%d%%\n", 
      time_buffer, cpu_delta.total, cpu_delta.idle, loadavg, net_delta.rx_bytes, rxload, net_delta.tx_bytes, txload);
    //Save actual sample as old for next iteration
    cpu_old = cpu_actual;
    net_old = net_actual;
    //Dont block context switches, let the process sleep for some time
    sleep(period);
  }
  fclose(log_fp);
return (0);
}

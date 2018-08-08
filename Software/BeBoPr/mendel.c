/** \file
	\brief Main file - this is where it all starts, and ends
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>

#include "heater.h"
#include "bebopr.h"
#include "mendel.h"
#include "gcode_process.h"
#include "gcode_parse.h"
#include "limit_switches.h"
#include "pruss_stepper.h"
#include "comm.h"
#include "debug.h"
#include "pruss.h"
#include "timestamp.h"


#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>


static int arm_init( void)
{
  if (geteuid() != 0) {
    // Only root can set scheduling parameters, not running as root
    // this may create all kinds of problems later on, so bail out!
    fprintf( stderr, "FATAL ERROR - This program can only be run as root!\n");
    exit( EXIT_FAILURE);
  }

  int policy = MENDEL_SCHED;
  int prio_max = sched_get_priority_max( policy);
  int prio_min = sched_get_priority_min( policy);

  struct sched_param sp = { 
    .sched_priority = MENDEL_PRIO
  };
  // Set realtime process scheduling using the Round Robin scheduler
  // with absolute priority halfway between min and max.
  if (sched_setscheduler( 0, policy, &sp) < 0) {
  }
  fprintf( stderr, "Scheduler set to %d, priority to %d (min:%d,max:%d)\n", policy, sp.sched_priority, prio_min, prio_max);

  struct timespec clock_resolution;
  clock_getres( CLOCK_MONOTONIC, &clock_resolution);
  if (clock_resolution.tv_sec == 0) {
    printf( "Clock resolution = %ld ns.\n", clock_resolution.tv_nsec);
  } else {
    printf( "Clock resolution = %ld.%09ld s.\n", clock_resolution.tv_sec, clock_resolution.tv_nsec);
  }
  timestamp_init();
  
  return 0;
}

static void signal_handler( int signal)
{
  fprintf( stderr, "Terminating on signal %d\n", signal);
  exit( EXIT_SUCCESS);
}

/// Startup code, run when we come out of reset
int init( void)
{
  int result;

  signal( SIGINT, signal_handler);
  signal( SIGHUP, signal_handler);
  signal( SIGTERM, signal_handler);

  // set up arm & linux specific stuff
  result = mendel_sub_init( "arm", arm_init);
  if (result != 0) {
    return result;
  }
  // configure
  result = mendel_sub_init( "bebopr (early)", bebopr_pre_init);
  if (result != 0) {
    return result;
  }
#ifndef NO_COMM_LAYER
  // keep connection alive
  result = mendel_sub_init( "comm", comm_init);
  if (result != 0) {
    return result;
  }
#endif
  // set up limit switches
  result = mendel_sub_init( "limsw", limsw_init);
  if (result != 0) {
    return result;
  }
  // This initializes the complete analog subsystem!
  result = mendel_sub_init( "heater", heater_init);
  if (result != 0) {
    return result;
  }
  // enable i/o power
  result = mendel_sub_init( "bebopr (late)", bebopr_post_init);
  if (result != 0) {
    return result;
  }
  // This initializes the trajectory code and PRUSS
  result = mendel_sub_init( "gcode_process", gcode_process_init);
  if (result != 0) {
    return result;
  }
#if 0
  /*
   *  Prevent problems on 3.8 kernels causing lockup due to I2C timeouts
   *  Rationale: The kernel tries to prevent starvation of low priority
   *  tasks. If RT runtime exceeds the sched_rt_runtime_us value (which
   *  defaults to 950 ms), RT scheduling is disabled for a while.
   *  "[sched_delayed] sched: RT throttling activated" will be logged.
   *  This causes the i2c controller that accesses the tps65217 to time-out
   *  and the system locks up. Even the heartbeat stops flashing.
   *
   *  FIXME: restore old value at exit!
   */
  system( "/sbin/sysctl -w kernel.sched_rt_runtime_us=-1");
#endif  
  return 0;
}

int mendel_thread_create( const char* name, pthread_t* restrict thread, const pthread_attr_t* restrict attr,
			  void* (*worker_thread)( void*), void* restrict arg)
{
  fprintf( stderr, "=== Creating %s_thread...", name);
  int result = pthread_create( thread, attr, worker_thread, arg);
  if (result == 0) {
    fprintf( stderr, "done ===\n");
    usleep( 1000);
  } else {
    fprintf( stderr, "failed with error=%d ===\n", result);
  }
  return result;
}

int mendel_sub_init( const char* name, int (*subsys)( void))
{
  fprintf( stderr, "Starting '%s' init ...\n", name);
  int result = subsys();
  if (result != 0) {
    fprintf( stderr, "... '%s' init failed with code %d\n", name, result);
  } else {
    fprintf( stderr, "... '%s' init was successfull\n", name);
  }
  return result;
}

static int normal_exit = 0;

void mendel_exit( void)
{
  if (normal_exit) {
    while (!pruss_queue_empty() || pruss_stepper_busy()) {
      usleep( 1000);
    }
  }
  pruss_queue_exit();
  fprintf( stderr, "mendel_exit called, waiting for output buffers to be flushed\n");
  usleep( 2 * 1000000);
}




/// this is where it all starts, and ends
///
/// just run init() that starts all threads, then run an endless loop where we pass characters from the serial RX buffer to gcode_parse_char()
// FIXME: This can now also be programmed as a (blocking) thread?
// FIXME: Implement proper program termination and un-init functions.
int main ( int argc, const char* argv[])
{
  extern const char* mendel_date;
  extern const int   mendel_version;
  extern const int   mendel_build;  
  fprintf( stderr, "Starting '%s' version %d.%d.%d (%s).\n",
	  argv[ 0], mendel_version, FW_VERSION, mendel_build, mendel_date);
  if (debug_flags) {
    fprintf( stderr, "Starting with debug flags set to 0x%04x (%u)\n",
	    debug_flags, debug_flags);
  }
  if (init() != 0) {
    fprintf( stderr, "Initialization failed, terminating.\n");
    exit( EXIT_FAILURE);
  }

  atexit( mendel_exit);
  
  // say hi to host
  printf( "start\nok\n");
  fprintf( stderr, "Starting main loop...\n");


  const char* socket_name = "/tmp/socket";
  remove(socket_name);
  int socket_fd;
  struct sockaddr_un name;
  /* Create the socket. */
  socket_fd = socket (PF_LOCAL, SOCK_STREAM, 0);
  /* Indicate that this is a server. */
  name.sun_family = AF_LOCAL;
  strcpy (name.sun_path, socket_name);
  bind (socket_fd, (struct sockaddr*)&name, SUN_LEN (&name));
  /* Listen for connections. */
  listen (socket_fd, 5);  
  
  
  int client_sent_quit_message;
  struct sockaddr_un client_name;
  socklen_t client_name_len;
  int client_socket_fd;
  do{
    /* Accept a connection. */
    client_socket_fd = accept (socket_fd, (struct sockaddr*)&client_name, &client_name_len);
    /* Handle the connection. */
    client_sent_quit_message = server (client_socket_fd);
    close (client_socket_fd);
  }while (!client_sent_quit_message);

  close (socket_fd);
  unlink (socket_name);
  normal_exit = 1;
  exit( EXIT_SUCCESS);
}

int server (int client_socket)
{
  int length=30;
  char recvBuff[100];
  char * sendBuff_1 = "busy";
  char * sendBuff_2 = "idle";
  while (1) {
    usleep( 1000);
    process_gcode_command( NULL);	// flush last command
    usleep( 1000);
    if(read(client_socket, recvBuff, length)==0) 
      return 0;
    printf("msg=%s\n", recvBuff);	
    if(strcmp(recvBuff, "check_status")==0)
    {
      if(pruss_stepper_busy())
      {	  
        write(client_socket, sendBuff_1, strlen(sendBuff_1)); 
      }
      else
      {
        write(client_socket, sendBuff_2, strlen(sendBuff_2));
      }
    }
    else if (strcmp(recvBuff, "quit")==0)
      return 1;
    else
    {
      char* p = recvBuff;
      while (*p) {
        gcode_parse_char( *p++);
      }
      process_gcode_command( NULL);
    }
  }
}
// -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-
// ex:ts=4:sw=4:sts=4:et
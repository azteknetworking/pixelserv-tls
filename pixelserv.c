/*
* pixelserv.c a small mod to public domain server.c -- a stream socket server demo
* from http://beej.us/guide/bgnet/
* single pixel http string from http://proxytunnel.sourceforge.net/pixelserv.php
*/

#include "util.h"
#include "socket_handler.h"

#include <sys/wait.h>   // waitpid()

#ifdef DROP_ROOT
# include <pwd.h>       // getpwnam()
#endif

#include <fcntl.h>      // fcntl() and related

#ifdef TEST
# include <arpa/inet.h> // inet_ntop()
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "certs.h"
#include "logger.h"

#ifdef USE_PTHREAD
#include <pthread.h>
#endif
#include <linux/version.h>

void signal_handler(int sig)
{
  if (sig != SIGTERM
   && sig != SIGUSR1
#ifdef DEBUG
   && sig != SIGUSR2
#endif
  ) {
    log_msg(LGG_WARNING, "Ignoring unsupported signal number: %d", sig);
    return;
  }
#ifdef DEBUG
  if (sig == SIGUSR2) {
    log_msg(LGG_INFO, "Main process caught signal %d near line number %lu of file %s", sig, LINE_NUMBER, __FILE__);
  } else {
#endif
    if (sig == SIGTERM) {
      // Ignore this signal while we are quitting
      signal(SIGTERM, SIG_IGN);
    }

    // log stats
    char* stats_string = get_stats(0, 0);
    log_msg(LGG_CRIT, "%s", stats_string);
    free(stats_string);

    if (sig == SIGTERM) {
      // exit program on SIGTERM
      log_msg(LOG_NOTICE, "exit on SIGTERM");
      exit(EXIT_SUCCESS);
    }
#ifdef DEBUG
  }
#endif
  return;
}

const char *tls_pem = DEFAULT_PEM_PATH;
int tls_ports[MAX_TLS_PORTS] = {0};
int num_tls_ports = 0;
STACK_OF(X509_INFO) *cachain = NULL;
struct Global *g;
cert_tlstor_t cert_tlstor;
#ifdef USE_PTHREAD
pthread_t certgen_thread;
#endif

int main (int argc, char* argv[]) // program start
{
  int sockfd = 0;  // listen on sock_fd
  int new_fd = 0;  // new connection on new_fd
  struct sockaddr_storage their_addr;  // connector's address information
  socklen_t sin_size;
  int yes = 1;
  char* version_string;
  time_t select_timeout = DEFAULT_TIMEOUT;
  time_t http_keepalive = DEFAULT_KEEPALIVE;
  int rv = 0;
  char* ip_addr = DEFAULT_IP;
  int use_ip = 0;
  struct addrinfo hints, *servinfo;
  int error = 0;
  int pipefd[2];  // IPC pipe ends (0 = read, 1 = write)
  response_struct pipedata = { FAIL_GENERAL, { 0 }, 0.0, 0 };
  char* ports[MAX_PORTS];
  ports[0] = DEFAULT_PORT;
  ports[1] = SECOND_PORT;
  tls_ports[0] = atoi(SECOND_PORT);
  char *port = NULL;
  fd_set readfds;
  fd_set selectfds;
  int sockfds[MAX_PORTS];
  int select_rv = 0;
  int nfds = 0;
  int num_ports = 0;
  int i;
#ifdef IF_MODE
  char *ifname = "";
  int use_if = 0;
#endif
#ifdef DROP_ROOT
  char *user = DEFAULT_USER;  // used to be long enough
  struct passwd *pw = 0;
#endif
  char* stats_url = DEFAULT_STATS_URL;
  char* stats_text_url = DEFAULT_STATS_TEXT_URL;
  int do_204 = 1;
#ifndef TEST
  int do_foreground = 0;
#endif // !TEST
  int do_redirect = 1;
#ifdef DEBUG
  int warning_time = 0;
#endif //DEBUG
  int max_num_threads = DEFAULT_THREAD_MAX;

  SET_LINE_NUMBER(__LINE__);

  // command line arguments processing
  for (i = 1; i < argc && error == 0; ++i) {
    if (argv[i][0] == '-') {
      // handle arguments that don't require a subsequent argument
      switch (argv[i][1]) {
        case '2': do_204 = 0;                                 continue;
#ifndef TEST
        case 'f': do_foreground = 1;                          continue;
#endif // !TEST
        case 'r': /* deprecated - ignoring */                 continue;
        case 'R': do_redirect = 0;                            continue;
        // no default here because we want to move on to the next section
      }
      // handle arguments that require a subsequent argument
      if ((i + 1) < argc) {
        // switch on parameter letter and process subsequent argument
        switch (argv[i++][1]) {
          case 'l':
            if ((logger_level)atoi(argv[i]) > LGG_DEBUG
                || (logger_level)atoi(argv[i]) < 0)
              error = 1;
            else
              log_set_verb((logger_level)atoi(argv[i]));
            continue;
#ifdef IF_MODE
          case 'n':
            ifname = argv[i];
            use_if = 1;
          continue;
#endif
          case 'o':
            errno = 0;
            select_timeout = strtol(argv[i], NULL, 10);
            if (errno || select_timeout <= 0) {
              error = 1;
            }
          continue;
          case 'O':
            errno = 0;
            http_keepalive = strtol(argv[i], NULL, 10);
            if (errno || http_keepalive <= 0) {
              error = 1;
            }
          continue;
          case 'k':
            if (num_tls_ports < MAX_TLS_PORTS)
              tls_ports[num_tls_ports++] = atoi(argv[i]);
            else
              error = 1;
              // fall through to case 'p'
          case 'p':
            if (num_ports < MAX_PORTS) {
              ports[num_ports++] = argv[i];
            } else {
              error = 1;
            }
          continue;
          case 's': stats_url = argv[i];                      continue;
          case 't': stats_text_url = argv[i];                 continue;
          case 'T':
            errno = 0;
            max_num_threads = strtol(argv[i], NULL, 10);
            if (errno || max_num_threads <= 0) {
              error = 1;
            }
          continue;
#ifdef DROP_ROOT
          case 'u': user = argv[i];                           continue;
#endif
#ifdef DEBUG
          case 'w':
            errno = 0;
            warning_time = strtol(argv[i], NULL, 10);
            if (errno || warning_time <= 0) {
              error = 1;
            }
          continue;
#endif //DEBUG
          case 'z':
            tls_pem = argv[i];
          continue;
          default:  error = 1;                                continue;
        }
      } else {
        error = 1;
      }
    } else if (use_ip == 0) {  // assume its a listening IP address
      ip_addr = argv[i];
      use_ip = 1;
    } else {
      error = 1;  // fix bug with 2 IP like args
    } // -
  } // for

  SET_LINE_NUMBER(__LINE__);

  if (error) {
    printf("%s: %s compiled: " __DATE__ " " __TIME__ "\n"
           "Usage: pixelserv-tls [OPTION]" "\n"
           "options:" "\n"
           "\t" "ip or hostname (default: 0.0.0.0)" "\n"
           "\t" "-2 (disable HTTP 204 reply to generate_204 URLs)" "\n"
#ifndef TEST
           "\t" "-f (stay in foreground/don't daemonize)" "\n"
#endif // !TEST
           "\t" "-k https_port (default: "
           SECOND_PORT
           ")" "\n"
           "\t" "-l level (0:critical 1:error<default> 2:warning 3:notice 4:info 5:debug)" "\n"
#ifdef IF_MODE
           "\t" "-n iface_name (default: all interfaces)" "\n"
#endif // IF_MODE
           "\t" "-o select_timeout (default: %ds)" "\n"
           "\t" "-O keep_alive_duration (for HTTP/1.1 connections; default: %ds)" "\n"
           "\t" "-p http_port (default: "
           DEFAULT_PORT
           ")" "\n"
           "\t" "-R (disable redirect to encoded path in tracker links)" "\n"
           "\t" "-s /relative_stats_html_URL (default: "
           DEFAULT_STATS_URL
           ")" "\n"
           "\t" "-t /relative_stats_txt_URL (default: "
           DEFAULT_STATS_TEXT_URL
           ")" "\n"
           "\t" "-T max_service_threads (default: %d)\n"
#ifdef DROP_ROOT
           "\t" "-u user (default: \"nobody\")" "\n"
#endif // DROP_ROOT
#ifdef DEBUG
           "\t" "-w warning_time (warn when elapsed connection time exceeds value in msec)" "\n"
#endif //DEBUG
           "\t" "-z path_to_https_certs (default: "
           DEFAULT_PEM_PATH
           ")" "\n"
           , argv[0], VERSION, DEFAULT_TIMEOUT, DEFAULT_KEEPALIVE, DEFAULT_THREAD_MAX);
    exit(EXIT_FAILURE);
  }

  SET_LINE_NUMBER(__LINE__);

#ifndef TEST
  if (!do_foreground && daemon(0, 0)) {
    log_msg(LGG_ERR, "failed to daemonize, exit: %m");
    exit(EXIT_FAILURE);
  }
#endif
  SET_LINE_NUMBER(__LINE__);

  openlog("pixelserv-tls", LOG_PID | LOG_CONS | LOG_PERROR, LOG_DAEMON);
  version_string = get_version(argc, argv);
  if (version_string) {
    log_msg(LGG_CRIT, "%s", version_string);
    free(version_string);
  } else {
    exit(EXIT_FAILURE);
  }

  SET_LINE_NUMBER(__LINE__);

  SSL_library_init();
#ifdef USE_PTHREAD
  ssl_init_locks();
#endif
  mkfifo(PIXEL_CERT_PIPE, 0600);
  pw = getpwnam(user);
  chown(PIXEL_CERT_PIPE, pw->pw_uid, pw->pw_gid);
  {
    char *fname = malloc(PIXELSERV_MAX_PATH);
    strcpy(fname, tls_pem);
    strcat(fname, "/ca.crt");
    FILE *fp = fopen(fname, "r");
    X509 *cacert = X509_new();
    if(fp == NULL || PEM_read_X509(fp, &cacert, NULL, NULL) == NULL)
      log_msg(LGG_ERR, "Failed to open/read ca.crt");
    else {
      EVP_PKEY * pubkey = X509_get_pubkey(cacert);
      if (X509_verify(cacert, pubkey) <= 0)
      {
        BIO *bioin; int fsz; char *cafile;

        if (fseek(fp, 0L, SEEK_END) < 0)
          log_msg(LGG_ERR, "Failed to seek ca.crt");
        fsz = ftell(fp);
        cafile = malloc(fsz);
        fseek(fp, 0L, SEEK_SET);
        fread(cafile, 1, fsz, fp);

        bioin = BIO_new_mem_buf(cafile, fsz);
        if (!bioin)
          log_msg(LGG_ERR, "Failed to create new BIO mem buffer");

        cachain = PEM_X509_INFO_read_bio(bioin, NULL, NULL, NULL);
        if (!cachain)
          log_msg(LGG_ERR, "Failed to read CA chain from ca.crt");
        BIO_free(bioin);
        free(cafile);
      }
      fclose(fp);
      free(fname);
      EVP_PKEY_free(pubkey);
      X509_free(cacert);

      cert_tlstor.pem_dir = tls_pem;
  #ifndef USE_PTHREAD
      if(fork() == 0){
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
  #ifdef DEBUG
        sigaddset(&mask, SIGUSR2);
  #endif
        sigprocmask(SIG_SETMASK, &mask, NULL);
        cert_generator((void*)&cert_tlstor);
        exit(0);
      }
  #else
      pthread_create(&certgen_thread, NULL, cert_generator, (void*)&cert_tlstor);
  #endif
    }
  }

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;  // AF_UNSPEC - AF_INET restricts to IPV4
  hints.ai_socktype = SOCK_STREAM;
  if (!use_ip) {
    hints.ai_flags = AI_PASSIVE;  // use my IP
  }

  //no -p no -k
  if (!num_ports) {
    num_ports = 2;
    num_tls_ports = 1;
  } else if (!num_tls_ports) {
  //no -k
    tls_ports[num_tls_ports++] = atoi(SECOND_PORT);
    ports[num_ports++] = SECOND_PORT;  
  } else if (num_ports == num_tls_ports)
  //no -p
    ports[num_ports++] = DEFAULT_PORT;    

  // clear the set
  FD_ZERO(&readfds);

  SET_LINE_NUMBER(__LINE__);

  for (i = 0; i < num_ports; i++) {
    port = ports[i];

    rv = getaddrinfo(use_ip ? ip_addr : NULL, port, &hints, &servinfo);
    if (rv) {
      log_msg(LGG_ERR, "getaddrinfo: %s", gai_strerror(rv) );
      exit(EXIT_FAILURE);
    }

    if ( ((sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol)) < 1)
      || (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)))
      || (setsockopt(sockfd, SOL_TCP, TCP_NODELAY, &yes, sizeof(int)))  // send short packets straight away
#ifdef IF_MODE
      || (use_if && (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname))))  // only use selected i/f
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,16,0)
      || (setsockopt(sockfd, SOL_TCP, TCP_FASTOPEN, &yes, sizeof(int)))
#endif
      || (bind(sockfd, servinfo->ai_addr, servinfo->ai_addrlen))
      || (listen(sockfd, BACKLOG))
      || (fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL) | O_NONBLOCK))  // set non-blocking mode
       ) {
#ifdef IF_MODE
      log_msg(LGG_ERR, "Abort: %m - %s:%s:%s", ifname, ip_addr, port);
#else
      log_msg(LOG_ERR, "Abort: %m - %s:%s", ip_addr, port);
#endif
      exit(EXIT_FAILURE);
    }

    SET_LINE_NUMBER(__LINE__);

    sockfds[i] = sockfd;
    // add descriptor to the set
    FD_SET(sockfd, &readfds);
    if (sockfd > nfds) {
      nfds = sockfd;
    }

    freeaddrinfo(servinfo); // all done with this structure
#ifdef IF_MODE
    log_msg(LGG_CRIT, "Listening on %s:%s:%s", ifname, ip_addr, port);
#else
    log_msg(LGG_CRIT, "Listening on %s:%s", ip_addr, port);
#endif
  }

  SET_LINE_NUMBER(__LINE__);

  // set up signal handling
  {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    // set signal handler for termination
    if (sigaction(SIGTERM, &sa, NULL)) {
      log_msg(LOG_ERR, "SIGTERM %m");
      exit(EXIT_FAILURE);
    }

    // attempt to set SIGCHLD to ignore
    // in K26 this should cause children to be automatically reaped on exit
    // in K24 it will accomplish nothing, so we still need to use waitpid()
    if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
      log_msg(LGG_WARNING, "SIGCHLD %m");
    }

    // set signal handler for info
    sa.sa_flags = SA_RESTART; // prevent EINTR from interrupted library calls
    if (sigaction(SIGUSR1, &sa, NULL)) {
      log_msg(LOG_ERR, "SIGUSR1 %m");
      exit(EXIT_FAILURE);
    }
#ifdef DEBUG
    // set signal handler for debug
    sa.sa_flags = SA_RESTART; // prevent EINTR from interrupted library calls
    if (sigaction(SIGUSR2, &sa, NULL)) {
      log_msg(LOG_ERR, "SIGUSR2 %m");
      exit(EXIT_FAILURE);
    }
#endif
  }

  SET_LINE_NUMBER(__LINE__);

#ifdef DROP_ROOT // no longer fatal error if doesn't work
  if ( (pw = getpwnam(user)) == NULL ) {
    log_msg(LGG_WARNING, "Unknown user \"%s\"", user);
  }
  else if ( setuid(pw->pw_uid) ) {
    log_msg(LGG_WARNING, "setuid %d: %m", pw->pw_uid);
  }
#endif

  SET_LINE_NUMBER(__LINE__);

  // cause failed pipe I/O calls to result in error return values instead of
  //  SIGPIPE signals
  signal(SIGPIPE, SIG_IGN);

  SET_LINE_NUMBER(__LINE__);

  // open pipe for children to use for writing data back to main
  if (pipe(pipefd) == -1) {
    log_msg(LOG_ERR, "pipe() error: %m");
    exit(EXIT_FAILURE);
  }
  // set non-blocking read mode
  // note that writes are left as blocking because otherwise weird things happen
  if (fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK) == -1) {
    log_msg(LOG_ERR, "fcntl() error setting O_NONBLOCK on read end of pipe: %m");
    exit(EXIT_FAILURE);
  }

  SET_LINE_NUMBER(__LINE__);

  // also have select() monitor the read end of the stats pipe
  FD_SET(pipefd[0], &readfds);
  // note if pipe read descriptor is the largest fd number we care about
  if (pipefd[0] > nfds) {
    nfds = pipefd[0];
  }

  // nfds now contains the largest fd number of interest;
  //  increment by 1 for use with select()
  ++nfds;

  sin_size = sizeof their_addr;

  SET_LINE_NUMBER(__LINE__);

  struct Global _g = {
        argc,
        argv,
        select_timeout,
        http_keepalive,
        pipefd[1],
        stats_url,
        stats_text_url,
        do_204,
        do_redirect,
#ifdef DEBUG
        warning_time,
#endif
  };
  g = &_g;

  // main accept() loop
  while(1) {

    SET_LINE_NUMBER(__LINE__);

    // only call select() if we have something more to process
    if (select_rv <= 0) {
      // select() modifies its fd set, so make a working copy
      // readfds should not be referenced after this point, as it must remain
      //  intact
      selectfds = readfds;
      // NOTE: MACRO needs "_GNU_SOURCE"; without this the select gets
      //       interrupted with errno EINTR
      select_rv = TEMP_FAILURE_RETRY(select(nfds, &selectfds, NULL, NULL, NULL));
      if (select_rv < 0) {
        log_msg(LOG_ERR, "main select() error: %m");
        exit(EXIT_FAILURE);
      } else if (select_rv == 0) {
        // this should be pathological, as we don't specify a timeout
        log_msg(LGG_WARNING, "main select() returned zero (timeout?)");
        continue;
      }
    }

    SET_LINE_NUMBER(__LINE__);

    // find first socket descriptor that is ready to read (if any)
    // note that even though multiple sockets may be ready, we only process one
    //  per loop iteration; subsequent ones will be handled on subsequent passes
    //  through the loop
    for (i = 0, sockfd = 0; i < num_ports; i++) {
      if ( FD_ISSET(sockfds[i], &selectfds) ) {
        // select sockfds[i] for servicing during this loop pass
        sockfd = sockfds[i];
        --select_rv;
        break;
      }
    }

    SET_LINE_NUMBER(__LINE__);

    // if select() didn't return due to a socket connection, check for pipe I/O
    if (!sockfd && FD_ISSET(pipefd[0], &selectfds)) {
      // perform a single read from pipe
      rv = read(pipefd[0], &pipedata, sizeof(pipedata));
      if (rv < 0) {
        log_msg(LGG_WARNING, "error reading from pipe: %m");
      } else if (rv == 0) {
        log_msg(LGG_WARNING, "pipe read() returned zero");
      } else if (rv != sizeof(pipedata)) {
        log_msg(LGG_WARNING, "pipe read() got %d bytes, but %u bytes were expected - discarding", rv, (unsigned int)sizeof(pipedata));
      } else {
        // process response type
        switch (pipedata.status) {
          case FAIL_GENERAL:   ++err; break;
          case FAIL_TIMEOUT:   ++tmo; break;
          case FAIL_CLOSED:    ++cls; break;
          case FAIL_REPLY:     ++cly; break;
          case SEND_GIF:       ++gif; break;
          case SEND_TXT:       ++txt; break;
          case SEND_JPG:       ++jpg; break;
          case SEND_PNG:       ++png; break;
          case SEND_SWF:       ++swf; break;
          case SEND_ICO:       ++ico; break;
          case SEND_BAD:       ++bad; break;
          case SEND_STATS:     ++sta; break;
          case SEND_STATSTEXT: ++stt; break;
          case SEND_204:       ++noc; break;
          case SEND_REDIRECT:  ++rdr; break;
          case SEND_NO_EXT:    ++nfe; break;
          case SEND_UNK_EXT:   ++ufe; break;
          case SEND_NO_URL:    ++nou; break;
          case SEND_BAD_PATH:  ++pth; break;
          case SEND_POST:      ++pst; break;
          case SEND_HEAD:      ++hed; break;
          case SEND_OPTIONS:   ++opt; break;
          case ACTION_LOG_VERB:  log_set_verb(pipedata.verb); break;
          case ACTION_DEC_KCC: --kcc; break;
          default:
            log_msg(LOG_DEBUG, "conn_handler reported unknown response value: %d", pipedata.status);
        }
        
        switch (pipedata.ssl) {
          case SSL_HIT:        ++slh; break;
          case SSL_MISS:       ++slm; break;
          case SSL_ERR:        ++sle; break;
          case SSL_HIT_CLS:    ++slc; break;
          case SSL_NOT_TLS:    break;
          default:
            ++slu;
            log_msg(LGG_DEBUG, "conn_handler reported unknown ssl state: %d", pipedata.ssl);
        }
        count++;
        SET_LINE_NUMBER(__LINE__);

        if (pipedata.status < ACTION_LOG_VERB) {
          // count only positive receive sizes
          if (pipedata.rx_total <= 0) {
            log_msg(LOG_DEBUG, "pipe read() got nonsensical rx_total data value %d - ignoring", pipedata.rx_total);
          } else {
            // calculate average byte per request (avg) using
            static float favg = 0.0; 
            static int favg_cnt = 0;
            favg = ema(favg, pipedata.rx_total, &favg_cnt);
            avg = favg + 0.5;
            // look for a new high score
            if (pipedata.rx_total > rmx)
              rmx = pipedata.rx_total;
          }

          if (pipedata.status != FAIL_TIMEOUT) {
            // calculate average process time (tav) using
            static float ftav = 0.0;
            static int ftav_cnt = 0;
            ftav = ema(ftav, pipedata.run_time, &ftav_cnt);
            tav = ftav + 0.5;
            // look for a new high score, adding 0.5 for rounding
            if (pipedata.run_time + 0.5 > tmx)
              tmx = (pipedata.run_time + 0.5);
          }
        } else if (pipedata.status == ACTION_DEC_KCC) {
          static int kvg_cnt = 0;
          kvg = ema(kvg, pipedata.krq, &kvg_cnt);
          if (pipedata.krq > krq)
             krq = pipedata.krq;
        }
      }
      --select_rv;
      continue;
    }

    SET_LINE_NUMBER(__LINE__);

    // if select() returned but no fd's of interest were found, give up
    // note that this is bad because it means that select() will probably never
    //  block again because something will always be waiting on the unhandled
    //  file descriptor
    // on the other hand, this should be a pathological case unless something is
    //  added to FD_SET that is not checked before this point
    if (!sockfd) {
      log_msg(LGG_WARNING, "select() returned a value of %d but no file descriptors of interest are ready for read", select_rv);
      // force select_rv to zero so that select() will be called on the next
      //  loop iteration
      select_rv = 0;
      continue;
    }

    new_fd = accept(sockfd, (struct sockaddr *) &their_addr, &sin_size);
    if (new_fd < 0) {
      if (errno == EAGAIN
       || errno == EWOULDBLOCK) {
        // client closed connection before we got a chance to accept it
        log_msg(LGG_DEBUG, "accept: %m");
        count++;
        cls++;
      } else {
        log_msg(LGG_WARNING, "accept: %m");
      }
      continue;
    }

    if (kcc >= max_num_threads) {
        count++;
        clt++;
        close(new_fd);
        continue;
    }
    SET_LINE_NUMBER(__LINE__);

    conn_tlstor_struct *conn_tlstor = malloc(sizeof(conn_tlstor_struct));
    conn_tlstor->new_fd = new_fd;

#ifdef USE_PTHREAD
    pthread_t conn_thread;
    pthread_attr_t conn_attr;
    pthread_attr_init(&conn_attr);
    pthread_attr_setdetachstate(&conn_attr, PTHREAD_CREATE_DETACHED);
    int err;
    if ((err=pthread_create(&conn_thread, &conn_attr, conn_handler, (void*)conn_tlstor))) {
      log_msg(LGG_ERR, "Failed to create conn_handler thread. err: %d", err);
      continue;
    }
#else
    if (fork() == 0) {
      // detach child from signal handler
      signal(SIGTERM, SIG_DFL); // default is kill?
      signal(SIGUSR1, SIG_DFL); // default is ignore?
#ifdef DEBUG
      signal(SIGUSR2, SIG_DFL); // default is ignore?
#endif
      // close unneeded file handles inherited from the parent process
      close(sockfd);

      // note that only the read end is closed
      // even main() should leave the write end open so that children can
      //  inherit it
      close(pipefd[0]);

      conn_handler( (void*)conn_tlstor );
      exit(0);
    } // end of forked child process

    SET_LINE_NUMBER(__LINE__);

    // this is guaranteed to be the parent process, as the child calls exit()
    //  above when it's done instead of proceeding to this point
    close(new_fd);  // parent doesn't need this
    free(conn_tlstor);
#endif // USE_PTHREAD

    if (++kcc > kmx)
      kmx = kcc;

    SET_LINE_NUMBER(__LINE__);

    // reap any zombie child processes that have exited
    // irony note: I wrote this while watching The Walking Dead :p
    for (errno = 0; waitpid(-1, 0, WNOHANG) > 0 || (errno && errno != ECHILD); errno = 0) {
      if (errno && errno != ECHILD) {
        log_msg(LGG_ERR, "waitpid() reported error: %m");
      }
    }

    SET_LINE_NUMBER(__LINE__);
  } // end of perpetual accept() loop

#ifdef USE_PTHREAD
  pthread_cancel(certgen_thread);
  pthread_join(certgen_thread, NULL);
#endif
  sk_X509_pop_free(cachain, X509_free);
  ssl_free_locks();
  return (EXIT_SUCCESS);
}

#include <time.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <arpa/ftp.h>
#include <arpa/telnet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>   
#include <pthread.h>
#include <zlib.h>

#include "ftpparse.h"
#include "dns.h"
#include "buffer.h"
#include "ip4.h"
#include "strerr.h"
#include "error.h"
#include "tiger.h"
#include "ubi_sLinkList.h"
#include "ubi_BinTree.h"

typedef struct {
  ubi_trNode  Node;
  char       *name;
  unsigned long long hash[3];
} done_t;

typedef struct
  {
  ubi_slNode node;
  char       dir[1];
  } todo_t;

typedef struct {
  ubi_trNode  node;
  char       *name;
  char       *path;
  long size;
  time_t mtime;
  char *id;
} dir_t;

typedef struct {
  stralloc    ip;           /* the IP adress we are connecting to string*/
  stralloc    name;         /* the name the user provided to us */
  stralloc    hostname;     /* name of the host we are connecting to */
  int         fd;           /* filedescriptor of the connecting socket */
  gzFile     *outputf;      /* filedescriptor of outputfile */ 
  FILE       *logf;         /* filedescriptor to log to */
  int         buf_read_cnt; /* our selfrolled buffering */
  char       *buf_read_ptr;
  char       *buf_read_buf;
  size_t      bufsiz; 
  ubi_slList  todo;   /* a lists of all dirs we still have to spider */
  ubi_trRoot  done;   /* a tree of dirs we have already spidered */
} ftpcon;

#define FATAL "ftpx: fatal: "

#define BUFSIZE 4096

#define DEBUG_DUPDIRS          (1 << 4)
#define DEBUG_VISITEDDIRS      (1 << 5)
#define DEBUG_VISITEDFILES     (1 << 6)
#define DEBUG_FTPDIALOG        (1 << 7)
#define DEBUG_FTPCOMMANDS      (1 << 8)
#define DEBUG_FTPLISTING       (1 << 9)
#define DEBUG_FTPLIST_PARSE    (1 << 10)
#define DEBUG_FTPDIRCHOOSING   (1 << 11)
long debug = DEBUG_FTPDIALOG|DEBUG_VISITEDDIRS|DEBUG_FTPCOMMANDS;

extern int h_errno;

struct	sockaddr_in hisctladdr;
struct	sockaddr_in data_addr;
int	data = -1;
int	abrtflag = 0;
struct	sockaddr_in myctladdr;
off_t	restart_point = 0;


int log = 0;

FILE    *debugf;
FILE    *logf;
FILE	*cin, *cout;

/* you many want to change the following information for your network */

#define TCP_PORT   21
#define MAXLINE 512

static int doneCompareFunc( ubi_trItemPtr ItemPtr, ubi_trNodePtr NodePtr )
     /* ------------------------------------------------------------------------ **
      * If we are going to sort the data, we will need to be able to compare the
      * data.  That's what this function does.  A pointer to this function will
      * be stored in the tree header.
      *
      *  Input:  ItemPtr - A pointer to the comparison data record.
      *                    Take a look at the Insert(), Find() and Locate()
      *                    functions.  They all take a parameter of type
      *                    ubi_trItemPtr.  This is a pointer to the data to be
      *                    compared.  In Find() and Locate(), it's a value that
      *                    you want to find within the tree.  In Insert() it's
      *                    a pointer to the part of the new node that contains
      *                    the data to be compared.
      *          NodePtr - This is a pointer to a node in the tree.
      *  Output: An integer in one of three ranges:
      *          < 0 indicates ItemPtr < (the data contained in) NodePtr
      *          = 0 indicates ItemPtr = (the data contained in) NodePtr
      *          > 0 indicates ItemPtr > (the data contained in) NodePtr
      * ------------------------------------------------------------------------ **
      */
{
  int i = 0;
  unsigned long long *A, *B;
  
  A = (unsigned long long *)ItemPtr;       
  B = ((done_t *)NodePtr)->hash; 
  
  do
    {
      if(*A < *B) return -1;
      if(*A > *B) return 1;
      A++;
      B++;
      i++;
    } while(i < 3);
  
  return 0;
  
} /* take */


/* char *tigerBase64( unsigned long long res)
 *
 * Take a 192 bit tiger hash and return a 
 * base64 encoded string of 16 bytes size. 
 * (so you only get 96 bits back - we do this by 
 * XORing the first and the second half)
 *
 * returns NULL if out of memory
 */

char *tigerBase64( unsigned long long *res)
{
  static char encodingTable [64] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f',
    'g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v',
    'w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/'
  };
  char *ret;
  int i, pos1, pos2;
  
  if((ret = malloc(17)) == NULL)
    {
      return NULL;
    }
  
  for(i = 0; i < 16; i++)
    {
      /* Uh */
      pos1 = pos2 = 0;
      pos1 = (res[0] >> (i * 2)) & 3;
      pos1 |= ((res[1] >> (i * 2)) & 3) << 2;
      pos1 |= ((res[2] >> (i * 2)) & 3) << 4;
      pos2 = (res[0] >> ((i * 2) + 32)) & 3;
      pos2 |= ((res[1] >> ((i * 2) + 32)) & 3) << 2;
      pos2 |= ((res[2] >> ((i * 2) + 32)) & 3) << 4;

      ret[i] = encodingTable[pos1 ^ pos2];
    }
  ret[16] = 0;
  
  return ret;
}

/* Send a command to the Server */
int sendcommand(ftpcon *ftp, char *command)
{
  if(debug & DEBUG_FTPCOMMANDS)
    {
      printf(">>> %s\n", command);
    }
  write(ftp->fd, command, strlen(command));
  write(ftp->fd, "\r\n", 2);

  return 0;
}

/* stolen fron UNP */
/* uh, this is not reentrant */
static ssize_t my_read(ftpcon *ftp, char *ptr)
{
  
  if (ftp->buf_read_cnt <= 0) 
    {
    again:
      if ( (ftp->buf_read_cnt = read(ftp->fd, ftp->buf_read_buf, ftp->bufsiz)) < 0) 
	{
	  if (errno == EINTR)
	    {
	      goto again;
	    }
	  perror("my_read:");
	  return(-1);
	} 
      else if (ftp->buf_read_cnt == 0)
	{
	  return(0);
	}
      ftp->buf_read_ptr = ftp->buf_read_buf;
    }
  
  ftp->buf_read_cnt--;
  *ptr = *ftp->buf_read_ptr++;
  return(1);
}                   

/* basically from UNP */
ssize_t readline(ftpcon *ftp, void *vptr, size_t maxlen)
{
  int n, rc;
  char c, *ptr;

  ptr = vptr;
  for (n = 1; n < maxlen; n++) 
    {
      if ( (rc = my_read(ftp, &c)) == 1) 
	{
	  *ptr++ = c;
	  if (c == '\n')
	    {
	      if(*(ptr-2) == '\r')
		{
		  /* We've got an "\r\n" so the line ends. chop it of and return */
		  ptr -= 2;;
		  break; 
		}
	      else
		{
		  /* The servers author doesn't care about rfcs and just sends "\n" */
		  ptr--;
		  break;
		}
	    }
	} 
      else if (rc == 0) 
	{
	  if (n == 1)
	    {
	      return(0);      /* EOF, no data read */
	    }
	  else
	    {
	      break;          /* EOF, some data was read */
	    }
	} 
      else
	{
	  return(-1);             /* error, errno set by read() */
	}
    }   
  
  *ptr = 0;       /* null terminate like fgets() */
  return(n);
} 

int readanswer(ftpcon *ftp)
{
  char answer[MAXLINE];
  int n;

  memset(answer, 0, MAXLINE);
  
  do
    {
      n = readline(ftp, answer, MAXLINE);;
      if((debug & DEBUG_FTPDIALOG) && (!(debug & DEBUG_FTPDIALOG)))
	{
	  printf("<<< %s\n", answer);
	}
    } while(answer[3] == '-' || answer[0] == ' ');
      if(debug & DEBUG_FTPCOMMANDS)
	{
	  printf("<<< %s\n", answer);
	}
 
  answer[3] = 0;
  n = atoi(answer);
  return n;
}

int get_list(ftpcon *ftp, char *path)
{
  struct ftpparse fp;
  struct sockaddr_in data_addr;
  struct sockaddr_in sa;
  ftpcon dataread = {{0},{0},{0},0};
  char *a, *p;
  char line[MAXLINE];
  char pathn[MAXLINE];
  int datasockfd, datareadsockfd;
  int i, len, read_cnt, remaining_bytes;
  unsigned long long int hash[3], hash_tmp[3];
  done_t   *newnode;
  todo_t   *newtodo;
  dir_t    *direntr;
  ubi_slList  dir;
   
  hash[0] = hash[1] = hash[2] = hash_tmp[0] = hash_tmp[1] = hash_tmp[2] = 0;

  ubi_slInitList(&dir);

  len = sizeof(sa);

  if(getsockname(ftp->fd, &sa, &len) < 0)
    {
      perror("clientsoc: getsockname");
      exit(0);
    }

  len = sizeof(struct sockaddr_in);
  
  memset((char *) &data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_addr = sa.sin_addr;
  
  if ((datasockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    perror("clientsoc: can't open stream socket"), exit(0);

  if((dataread.buf_read_buf = (char *)malloc(BUFSIZE)) == NULL)
    {
      perror("malloc:");
    }
  dataread.bufsiz = BUFSIZE;
  dataread.buf_read_cnt = 0;
  dataread.buf_read_ptr = NULL;
  

  if(bind(datasockfd, &data_addr, sizeof(data_addr)) < 0)
    {
      perror("can't bind to dataport");
      exit(0);
    }

  if(listen(datasockfd, 1) < 0)
    {
      perror("can't listen on dataport");
      exit(0);
    }

  if(getsockname(datasockfd, &data_addr, &len) < 0)
    {
      perror("clientsoc: getsockname");
      exit(0);
    }

  /* fixme i'm an overflow */
  sprintf(line, "CWD %s", path);
  sendcommand(ftp, line);
  if(readanswer(ftp) != 250)
    {
      return 1;
    }
  
  a = (char *)&data_addr.sin_addr;
  p = (char *)&data_addr.sin_port;
  sprintf(line, "PORT %d,%d,%d,%d,%d,%d",
	  (int)a[0] & 0xff, 
	  (int)a[1] & 0xff, 
	  (int)a[2] & 0xff, 
	  (int)a[3] & 0xff,
	  (int)p[0] & 0xff, 
	  (int)p[1] & 0xff);

  sendcommand(ftp, line);
  readanswer(ftp);

  sendcommand(ftp, "LIST");
  if( readanswer(ftp) == 150)
  {
    if((dataread.fd = accept(datasockfd,  &sa,  &len)) < 0)
      {
	perror("accept:");
	exit(1);
      }
    
    close(datasockfd);
    datasockfd = 0;

    /* Now read the Directory Listing  */
    do 
      {
	memset(&line, 0, MAXLINE);
	
	read_cnt = readline(&dataread, line, sizeof(line));
	
	remaining_bytes = read_cnt;
	/* Now we try parsing the Data we have gotten */
	/* find first "\r\n" */

	if(debug & DEBUG_FTPLISTING)
	  {
	    printf( "%s\n", line);
	  }

	len = strlen(line);

	/* We hash every line and xor it with the other line together
	 * to get a hash of the whole directory without having to keep
	 * the whole directory in memory 
	 */
	
	tiger(line, len, hash_tmp); 

	hash[0] ^= hash_tmp[0];
	hash[1] ^= hash_tmp[1];
	hash[2] ^= hash_tmp[2];

	ftpparse(&fp,&line,len);
	
	if(fp.namelen > 0)
	  {
	    if((direntr = malloc(sizeof(dir_t))) == NULL)
	      {
		perror("get_list:");
		exit(1);
	      }

	    if(fp.flagtryretr == 1)
	      {
		if((a = malloc(fp.namelen + 1)) == NULL)
		  { 
		    perror("get_dir:");
		    exit(1);
		  }
		strncpy(a, fp.name, fp.namelen);
		a[fp.namelen] = (char)0;
		direntr->name = a;
	      }
	    else
	      {
		direntr->name = NULL;
	      }

	    if(fp.flagtrycwd == 1)
	      {
		strncpy(pathn, path, MAXLINE);
		/* Fix me I'm an overflow */
		strncat(pathn, fp.name, fp.namelen);
		strcat(pathn, "/");
		
		direntr->path = strdup(pathn);
	      }
	    else
	      {
		direntr->path = NULL;
	      }

	    if(fp.idlen > 0)
	      {
		/* hey-ho the server gave us an ID! */
		if((a = malloc(fp.idlen + 1)) == NULL)
		  { 
		    perror("get_dir:");
		    exit(1);
		  }
		strncpy(a, fp.id, fp.idlen);
		a[fp.idlen] = (char)0;
		direntr->id = a;
	      }
	    else
	      {
		/* we should create an ID ourself - todo*/
		direntr->id = tigerBase64(hash_tmp);		
	      }
	    
	    direntr->size = fp.size;
	    direntr->mtime = fp.mtime;
	    
	    ubi_slAddTail(&dir,  &(direntr->node));
	  }

	if((debug & DEBUG_FTPLIST_PARSE)  || 
	   ((debug & DEBUG_FTPDIRCHOOSING) && fp.flagtrycwd == 1))
	{
	  for(i=0; i < fp.namelen; i++)
	    {
	      putchar(fp.name[i]);
	    }
	  
	  printf(" flagtrycwd=%d flagtryretr=%d sizetype=%d size=%ld mtimetype=%d idtype=%d ", 
		 fp.flagtrycwd, 
		 fp.flagtryretr,
		 fp.sizetype,
		 fp.size,
		 fp.mtimetype,
		 fp.idtype);
	  
	  for(i=0; i < fp.idlen; i++)
	    {
	      putchar(fp.id[i]);
	    }
	  
	  putchar('\n');
	}	

      } while(read_cnt > 0);

    readanswer(ftp);

    close(datareadsockfd);
    if(datasockfd)
      close(datasockfd);

    if(debug & DEBUG_VISITEDDIRS)
      {
	printf("%s: hash=%Lx%Lx%Lx\n", path, hash[0], hash[1], hash[2]);
      }

    /* add data to our list of visited dirs */
    newnode = malloc(sizeof(done_t));
    a = strdup(pathn);

    if( !newnode || !a)
      {
	perror( "get_list" );
	exit( EXIT_FAILURE );
      }
    
    newnode->hash[0] = hash[0];
    newnode->hash[1] = hash[1];
    newnode->hash[2] = hash[2];
    newnode->name = a;

    if( !ubi_trInsert( &ftp->done,      /* To which tree are we adding this?    */
                       newnode,        /* The new node to be added.            */
                       newnode->hash,  /* Points to the comparison field.      */
                       NULL )          /* Overwrites are not allowed.          */
      )
      {
	if(debug & DEBUG_DUPDIRS)
	  {
	    printf( "%s: duplicate (or empty) dir. not added.\n", path );
	  }
	free(a);
	free(newnode);
      }    
    else
      {
	
	gzprintf(ftp->outputf, "%s\n", path);
	/* Add key to our Database */
	while(ubi_slCount(&dir))
	  {
	    direntr = (dir_t *) ubi_slDequeue(&dir);
	    
	    if(direntr->path != NULL)
	      {
		/* add dir to our todolist */
		
		if((newtodo = (todo_t *) malloc( sizeof(todo_t) + strlen(direntr->path))) == NULL)
		  {
		    perror("newnode:");
		    exit(1);
		  }
		strcpy(newtodo->dir, direntr->path);
		
		ubi_slAddTail(&ftp->todo,  &(newtodo->node));	   	    
		
		free(direntr->path);
	      }
	
	    if(direntr->name != NULL)
	      {
		gzprintf(ftp->outputf, "+i%s,s%ld,m%ld,\t%s\n", 
			direntr->id, direntr->size, 
			direntr->mtime, direntr->name );
		free(direntr->name);
	      }  
	    free(direntr->id);
	    free(direntr);
	  }
      }
  }
  
  return 0;
}

/* Do complete handling of a server */
void spider_server(ftpcon *ftp)
{
  int   sockfd;
  struct sockaddr_in serv_addr;
  struct sockaddr_in sa;
  int len;
  char line[MAXLINE];
  todo_t *node; 

  len = sizeof(struct sockaddr_in);
  
  memset((char *) &serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = inet_addr(ftp->ip.s);
  serv_addr.sin_port = htons(TCP_PORT);
  

  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    perror("clientsoc: can't open stream socket"), exit(0);
  
  ftp->fd = sockfd;
  if((ftp->buf_read_buf = (char *)malloc(BUFSIZE)) == NULL)
    {
      perror("malloc:");
    }
  if (connect(ftp->fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
    perror("clientsoc: can't connect to server"), exit(0);
  
  if(getsockname(ftp->fd, &sa, &len) < 0)
    {
      perror("clientsoc: getsockname");
      exit(0);
    }
  
  if(setsockopt(ftp->fd, SOL_SOCKET, SO_KEEPALIVE, NULL, 0) < 0)
  if(getsockname(ftp->fd, &sa, &len) < 0)
    {
      perror("clientsoc: setsockopt");
      exit(0);
    }

  memset(&line, 0, MAXLINE);
  readanswer(ftp);

  sendcommand(ftp, "USER anonymous");
  if(readanswer(ftp) ==  331)
    {
      sendcommand(ftp, "PASS drt@ailis.de");
      readanswer(ftp);
    }

  sendcommand(ftp, "SYST");
  readanswer(ftp);

  sendcommand(ftp, "STAT");
  readanswer(ftp);

  sendcommand(ftp, "HELP");
  readanswer(ftp);

  sendcommand(ftp, "TYPE I");
  readanswer(ftp);

  get_list(ftp, "/");

  while(ubi_slCount(&ftp->todo))
    {
      node = (todo_t *) ubi_slDequeue(&ftp->todo);
      get_list(ftp, (char*) &node->dir);
      free(node);
    }
  
  close(ftp->fd);
}


/* handle_server(char *name) - process server <name>
 */

void handle_server(char *name)
{
  time_t timevar;
  ftpcon ftp = {{0},{0},{0},0};  /* the central information structure */ 
  char str[IP4_FMT+1];
  stralloc tmp = {0};        /* temp var */
  stralloc udn = {0};        /* the name the user provides to us */ 

  /*
   * init our data structures
   *
   */

  /* zero-out everything */
  memset(&ftp, 0, sizeof(ftpcon));

  ftp.bufsiz = BUFSIZE;

  /* init dynamic datastructures */
  ubi_slInitList(&ftp.todo);
  ubi_trInitTree( &ftp.done,       /* Pointer to the tree header           */
		  doneCompareFunc,  /* Pointer to the comparison function.  */
		  0 );              /* Don't allow overwrites or duplicates.*/


  /*
   * do name/ip resolving 
   *
   */
  stralloc_copys(&ftp.name, name);
  
  if (!stralloc_copys(&udn, name))
    strerr_die2x(111,FATAL,"out of memory");

  /* we just accept a fqdn, try to resolve it */
  dns_ip4(&tmp,&udn);

  stralloc_copyb(&ftp.ip, str, ip4_fmt(str,tmp.s));
  stralloc_0(&ftp.ip);

  /* now we do the reverse lookup */
  dns_name4(&ftp.hostname, tmp.s);


  /* open <servername>.gz to write filelistings to */
  stralloc_copy(&tmp, &ftp.name);
  stralloc_cats(&tmp, ".gz");
  stralloc_0(&tmp);
  
  if((ftp.outputf = gzopen(tmp.s, "wb9")) == NULL)
    {
      perror("open");
      exit(1);
    }
  
  /* write a nice header to our file */
  time(&timevar);
  gzprintf(ftp.outputf, "# %s# %s (%s = %s)\n", ctime(&timevar), 
	   ftp.name.s, ftp.hostname.s, ftp.ip.s);
  
  spider_server(&ftp);

  gzclose(ftp.outputf);

}

int main(int argc, char **argv)
{
  static char seed[128];

  /* feed some entropy in places which need them*/
  srandom((unsigned int)(((long long) getpid () *
			  (long long) time(0) *
			  (long long) getppid() * 
			  (long long) clock() % 0xffff))); 
  (int)seed[0] = getpid ();
  (int)seed[20] = time(0);
  (int)seed[40] = getppid();
  (int)seed[60] = clock();
  (int)seed[80] = random();
  dns_random_init(seed);

  if(argc == 2)
    handle_server(argv[1]);
  else
    handle_server("ftp.ccc.de");

  /*  
  pthread_t chld_thr;
  pthread_t chld_thr2;
  if(pthread_create (&chld_thr, NULL, spider_server, (void *) &ftp) != 0)
  perror("couldn't create thread:"); 
  if(pthread_create (&chld_thr2, NULL, spider_server, (void *) &ftp2) != 0)
  perror("couldn't create thread:");
  if(pthread_join(chld_thr, 0) != 0) 
  perror("couldn't create thread:");
  */

  return(0);

}

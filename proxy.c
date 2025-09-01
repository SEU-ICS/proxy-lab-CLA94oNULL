#include <stdio.h>
#include <string.h>
#include "csapp.h"

#define MAX_SIZE 8192
#define SBUF_SIZE 16384
#define MAX_THREAD 8

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

#define FREE_PORT 43032



typedef struct
{
    char host[MAX_SIZE];
    char port[MAX_SIZE];
    char path[MAX_SIZE];
}URL;

int URL_is_equal(URL* a, URL* b)
{
    if (strcmp(a->host,b->host))return 0;
    if (strcmp(a->port,b->port))return 0;
    if (strcmp(a->path,b->path))return 0;
    return 1;
}

void URL_copy(URL* a, URL* b)
{
    strcpy(a->host, b->host);
    strcpy(a->port, b->port);
    strcpy(a->path, b->path);
    return;
}



/////////////////   cache   ///////////////////

#define MAX_OBJECT_CNT 100
typedef struct
{
    int empty;
    int read_cnt;
    int last_visit_time;
    sem_t mutex, w;
    char data[MAX_OBJECT_SIZE];
    URL url;
} CACHE;
CACHE cache[MAX_OBJECT_CNT];
int current_time = 0;
int read_cnt;
sem_t smt_mutex, smt_w;

void cache_init()
{
    for (int i = 0; i < MAX_OBJECT_CNT; i++)
    {
        cache[i].empty = 1;
        Sem_init(&cache[i].mutex, 0, 1); // 单线程共享信号量 初值1
        Sem_init(&cache[i].w, 0, 1);
    }
}

void cache_read_begin(CACHE* c)
{
    P(&c->mutex);
    c->read_cnt++;
    if (c->read_cnt == 1) P(&c->w);
    V(&c->mutex);
}
void cache_read_end(CACHE* c)
{
    P(&c->mutex);
    c->read_cnt--;
    if (c->read_cnt == 0) V(&c->w);
    V(&c->mutex);
}
void cache_write_begin(CACHE* c)
{
    P(&c->w);
}
void cache_write_end(CACHE* c)
{
    V(&c->w);
}

void cache_update(CACHE* c)
{
    cache_write_begin(c);
    c->last_visit_time = ++current_time;
    cache_write_end(c);
}

CACHE* cache_find(URL* url)
{
    CACHE* target = NULL;
    for (int i = 0; i < MAX_OBJECT_CNT && target == NULL; i++)
    {
        cache_read_begin(&cache[i]);
        if (!cache[i].empty)
        if (URL_is_equal(url, &cache[i].url))
        target = &cache[i];
        cache_read_end(&cache[i]);
    }
    if (target != NULL)
    cache_update(target);
    return target;
}

CACHE* cache_fill(CACHE* target, URL* url, char *data)
{
    cache_write_begin(target);
    target->empty = 0;
    strcpy(target->data, data);
    URL_copy(&target->url, url);
    cache_write_end(target);
    cache_update(target);
}

void cache_save(URL* url, char* data)
{
    CACHE* target = NULL;
    // 判断是否有空位放入缓存
    for (int i = 0; i < MAX_OBJECT_CNT; i++)
    {
        cache_read_begin(&cache[i]);
        
        if (cache[i].empty) target = &cache[i];
        
        cache_read_end(&cache[i]);
        
        if (target != NULL)
        {
            cache_fill(target, url, data);
            return;
        }
    }
    
    for (int i = 0; i < MAX_OBJECT_CNT; i++)
    {
        cache_read_begin(&cache[i]);
        
        if (!cache[i].empty)
        {
            if (target == NULL)
            {
                target = &cache[i];
            }
            else if (cache[i].last_visit_time < target->last_visit_time)
            {
                target = &cache[i];
            }
        }
        
        cache_read_end(&cache[i]);
    }
    
    cache_fill(target, url, data);
    return;
}



void parse_url(char* str, URL* url)
{
    //// scheme://domain:port/path?query_string#fragment
    
    char *pt1, *pt2, *pt3;
    ////  忽略 "http://"
    pt1 = strstr(str, "//");
    pt1 += 2;
    
    //// 找端口
    pt2 = strchr(pt1, ':');
    
    //// 找地址
    pt3 = strchr(pt1, '/');
    
    // 端口
    if (pt2 != NULL)
    {
        strncpy(url->port, pt2 + 1, pt3 - pt2 - 1);
        strncpy(url->host, pt1, pt2 - pt1);
    }
    else
    {
        strcpy(url->port, "80");
        strncpy(url->host, pt1, pt3-pt1);
    }
    
    // 地址
    strcpy(url->path, pt3);
}

void read_client(rio_t *rio, URL *url, char *data) 
{
    char host[MAX_SIZE], other[MAX_SIZE], method[MAX_SIZE], urlstr[MAX_SIZE], protocol[MAX_SIZE];
    char text[MAX_SIZE];
    
    // GET http://??.com/index.html HTTP/1.0
    Rio_readlineb(rio, text, MAX_SIZE);
    sscanf(text, "%s %s %s\n", method, urlstr, protocol);
    parse_url(urlstr, url);
    sprintf(host, "Host: %s", url->host);
    
    // 只处理host和其他字段，跳过Agent Connection
    while (Rio_readlineb(rio, text, MAX_SIZE) > 0) 
    {
        if (!strcmp(text, "\r\n"))
            break;
        if (!strncmp(text, "Host", 4))
            strcpy(host, text);
        if (strncmp(text, "User-Agent", 10) && strncmp(text, "Connection", 10) && strncmp(text, "Proxy-Connection", 16)) 
            strcat(other, text);
    }
    
    sprintf(data, "%s %s HTTP/1.0\r\n"
                  "%s\r\n"
                  "%s\r\n"
                  "Connection: close\r\n"
                  "Proxy-Connection: close\r\n"
                  "%s\r\n",
                  method, url->path, host, user_agent_hdr, other);
}

void doit(int connfd) {
    rio_t rio;
    char line[MAX_SIZE];
    Rio_readinitb(&rio, connfd);
    
    URL url;
    char data[MAX_SIZE];
    read_client(&rio, &url, data);
    
    // 查找cache
    for (int i = 0; i < MAX_OBJECT_CNT; i++)
    {
        cache_read_begin(&cache[i]);
        if (!cache[i].empty)
        if (URL_is_equal(url, &cache[i].url))
        {
            Rio_writen(connfd, cache[i]->data, strlen(cache[i]->data));
            cache_read_end(&cache[i]);
            return;
        }
        cache_read_end(&cache[i]);
    }
 
    int serverfd = open_clientfd(url.host, url.port);
    if (serverfd < 0) printf("Connection failed!\n");
    
    rio_readinitb(&rio, serverfd);
    Rio_writen(serverfd, data, strlen(data));
    
    int len, total_len = 0;
    char cache_data[MAX_OBJECT_SIZE];
    char* cache_data_ptr = cache_data;
    while ((len = Rio_readlineb(&rio, line, MAX_SIZE)) > 0)
    {
        Rio_writen(connfd, line, len);
        total_len += len;
        if (total_len < MAX_OBJECT_SIZE)
        {
            strcpy(cache_data_ptr, line);
            cache_data_ptr += len;
        }
    }
    if (total_len < MAX_OBJECT_SIZE)
    {
        cache_save(&url, cache_data);
    }
    
    
    Close(serverfd);
}



/////////////   csapp: sbuf.c   /////////////

#include <pthread.h>
#include <semaphore.h>

typedef struct 
{
    int *buf;
    int n;
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} sbuf_t;

void sbuf_init(sbuf_t *sp, int n)
{
    sp->buf = Calloc(n, sizeof(int)); 
    sp->n = n;                       /* Buffer holds max of n items */
    sp->front = sp->rear = 0;        /* Empty buffer iff front == rear */
    Sem_init(&sp->mutex, 0, 1);      /* Binary semaphore for locking */
    Sem_init(&sp->slots, 0, n);      /* Initially, buf has n empty slots */
    Sem_init(&sp->items, 0, 0);      /* Initially, buf has zero data items */
}

void sbuf_deinit(sbuf_t *sp)
{
    Free(sp->buf);
}

void sbuf_insert(sbuf_t *sp, int item)
{
    P(&sp->slots);                          /* Wait for available slot */
    P(&sp->mutex);                          /* Lock the buffer */
    sp->buf[(++sp->rear)%(sp->n)] = item;   /* Insert the item */
    V(&sp->mutex);                          /* Unlock the buffer */
    V(&sp->items);                          /* Announce available item */
}

int sbuf_remove(sbuf_t *sp)
{
    int item;
    P(&sp->items);                          /* Wait for available item */
    P(&sp->mutex);                          /* Lock the buffer */
    item = sp->buf[(++sp->front)%(sp->n)];  /* Remove the item */
    V(&sp->mutex);                          /* Unlock the buffer */
    V(&sp->slots);                          /* Announce available slot */
    return item;
}







sbuf_t sbuf;
void solver(void* unknown)
{
    Pthread_detach(pthread_self());
    while(1)
    {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
    }
}

int main(int argc, char **argv)
{
    // printf("%s", user_agent_hdr);
    
    int listen_fd, connect_fd;
    socklen_t client_len;
    char hostname[MAX_SIZE], port[MAX_SIZE];
    
    struct sockaddr client_addr;
    
    if (argc != 2)
    {
        printf("error1\n");
        exit(1);
    }
    listen_fd = Open_listenfd(argv[1]);
    
    sbuf_init(&sbuf, SBUF_SIZE);
    cache_init();
    for(int i = 0; i < MAX_THREAD; i++)
    {
        pthread_t trd;
        Pthread_create(&trd, NULL, solver, NULL);
    }
    
    while(1)
    {
        client_len = sizeof(client_addr);
        connect_fd = Accept(listen_fd, &client_addr, &client_len);
        
        
        Getnameinfo(&client_addr, client_len, hostname, MAX_SIZE, port, MAX_SIZE, 0);
        
        printf("Connection Accepted: %s %s\n", hostname, port);
        sbuf_insert(&sbuf, connect_fd);
        // doit(connect_fd);
        // Close(connect_fd);
        
    }
    
    return 0;
}

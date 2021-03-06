/* spider.cpp
 * this file is the main entrance of the program which contains
 * main() function. It also defines global variables. It show us
 * the main frame of the program.
 */

#include "http.h"
#include "debug.h"
#include "config.h"
#include "spider.h"

/* init()
 * initialize global data
 */

void init()
{
    g_cnt = 0;
    g_sum_byte = 0;
    g_pending = 0;
    g_is_first_url = true;
    g_input = START_URL;
    g_keyword = KEYWORD;
    g_time_used = 0;
    g_nthread = 5;
}

/* Usage()
 * used to print the usage of this spider
 */

void Usage()
{
    printf("usage\n");
    printf("-h    --help    print the usage of this spider.                 default    0\n");
    printf("-n    --nurl    set the number of url you want to get.          default    INF\n");
    printf("-u    --url     set the first url you want to start fetch.      default    \"\"\n");
    printf("-k    --key     set the key word of the url to fetch web pages. default    \"\"\n");
    printf("-t    --timeout set timeout for epoll(waitque is empty).        default    20\n");
    printf("\nexample\n");
    printf("./spider -h\n");
    printf("./spider -n 1000 -u http://hi.baidu.com/xxx -k xxx\n");
    printf("./spider --url http://hi.baidu.com/xxx --key xxx\n");
    printf("./spider --url http://hi.baidu.com/xxx -t 15\n");
}

/* Parse()
 * It call getopt_long() function to parse command line arguments.
 */

int Parse(int argc, char **argv) 
{
    int c;
    int ret = 0;
    while((c = getopt_long(argc, argv, short_opt, long_opt, NULL)) != -1)
    {
        switch(c)
        {
            case 'h':
                return -2;
            case 'n':
                MAX_URL = atoi(optarg);
                break;
            case 'u':
                START_URL = string(optarg);
                break;
            case 'k':
                KEYWORD = string(optarg);
                break;
            case 't':
                TIMEOUT = atoi(optarg);
                break;
            default:
                return -1;
        }
        ret ++;
    }
    return ret;
}

/* generator()
 * this dead loop below deal with all events.(to every event, it will create a
 * new thread to execute).
 */

void generator()
{
    int retry_wait_times = 0;
    while(1) 
    {
        // max num of url we want to fetch
        if(g_cnt>=MAX_URL) 
            break;

        // int epoll_wait(int epfd, struct epoll_event * events, int maxevents, int timeout);
        int n = epoll_wait(g_epfd, g_events, 30, 2000);
        
        // set retry_wait_times 
        if(retry_wait_times >= TIMEOUT) 
        {
            char info[200];
            sprintf(info, "wait time out for retry %d times\n", TIMEOUT);
            PRINT(info);
            break;
        }
        if(n<0) 
        {
            perror("epoll_wait_error");
            break;
        }
        else if(n==0 && g_que.empty()) 
        {
            retry_wait_times++;
            continue;
        }
        retry_wait_times = 0;
        
        // create pthread for every event and call GetResponse().
        for(int i=0; i<n; i++) 
        {
            struct argument* arg = (struct argument*)g_events[i].data.ptr;
            
            pthread_attr_t pAttr;
            pthread_t      thread;
        
            pthread_attr_init(&pAttr);                                      // initialize pthread attribute
            pthread_attr_setstacksize(&pAttr, 8 * 1024 * 1024);             // 1M stack size
            pthread_attr_setdetachstate(&pAttr, PTHREAD_CREATE_DETACHED);   // or JOINABLE

            int pid = pthread_create(&thread, &pAttr, GetResponse, arg);

            if(pid != 0 ) 
            {
                perror("create thread");
                pthread_attr_destroy(&pAttr);
                continue;
            }
            pthread_attr_destroy(&pAttr);

            struct epoll_event ev;
            
            // delete used event with sockfd = arg->sockfd.
            if(epoll_ctl(g_epfd, EPOLL_CTL_DEL, arg->sockfd, &ev) == -1) 
            {
                epoll_ctl(g_epfd, EPOLL_CTL_MOD, arg->sockfd, &ev);
                perror("epoll_ctl_del");
                continue;
            }
        }
    }
}

/* start_run()
 * start from this main process
 */

void start_run()
{
    // create epoll fd(g_epfd) with maximum events 50
    g_epfd = epoll_create(50);

    int n = (g_que.size()>=g_nthread) ?  g_nthread : g_que.size();

    for(int i=0; i<n; i++) // foreach url we want fetch
    {
        pthread_mutex_lock(&quelock);
        URL url_t = g_que.front();
        g_que.pop();
        pthread_mutex_unlock(&quelock);
        
        int sockfd;
        
        // connect to web
        int retry_conn_times = 0;
        while(ConnectWeb(sockfd) < 0 && retry_conn_times < 5) 
        {
            sleep(1);
            retry_conn_times++;
        }

        if(retry_conn_times >= 5) 
        {
            perror("create socket");
            exit(1);
        }
        
        // setnoblock
        if(SetNoblocking(sockfd) < 0) 
        {
            perror("setnoblock");
            exit(1);
        }
        
        // sendrequest
        if(SendRequest(sockfd, url_t) < 0) 
        {
            perror("sendrequest");
            exit(1);
        }
        
        // initialize argument that the event want to pass to called function
        struct argument *arg=new struct argument;
        struct epoll_event ev;
        arg->url = url_t;
        arg->sockfd = sockfd;
        ev.data.ptr = arg;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;

        // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
        // enroll event by EPOLL_CTL_ADD mode
        if(epoll_ctl(g_epfd, EPOLL_CTL_ADD, sockfd, &ev) == -1) 
        { 
            perror("epoll_ctl_add");
            exit(1);
        }
    }

    generator(); // handle response
    close(g_epfd);
}

/* summery(timeval t_st, timeval t_ed)
 * print summery
 */

void summery(timeval t_st, timeval t_ed)
{
    struct tm *p;
    time_t ti;

    time(&ti);
    p = gmtime(&ti);
    g_time_used = Calc_Time_Sec(t_st, t_ed);

    // print summary infomations
    printf("\n                           STATISTICS\n");
    printf("----------------------------------------------------------------------\n");
    printf("fetch target:           %s\n", g_input.c_str());
    printf("totally urls fetched:   %d\n", g_cnt);
    printf("totally byte fetched:   %.2fKB\n", g_sum_byte/1024.0);
    printf("totally time cost:      %.2fs\n", g_time_used);
    printf("average download speed: %.2fKB/s\n", g_sum_byte/1024.0/g_time_used);
    printf("auther:                 szw\n");
    printf("date:                   %04d-%02d-%02d\n",(1900+p->tm_year), (1+p->tm_mon),p->tm_mday);  
    printf("time:                   %02d:%02d:%02d\n", (p->tm_hour+8)%24, p->tm_min, p->tm_sec);  
    printf("----------------------------------------------------------------------\n");
}

/* main()
 * main entrance. deal with the first url and create new pthread.
 */

int main(int argc, char **argv)
{
    // 1. parse arguments
    if(argc<1) 
    {
        printf("command argument error!\n");
        exit(1);
    }

    // parse argument by call Parse() function.
    int rec = Parse(argc, argv);
    if(rec == -1 || rec == 0) 
    {
        printf("command argument error!\n");
        Usage();
        exit(1);
    } 
    else if(rec == -2) 
    {
        Usage();
        exit(1);
    }

    // 2. prepare for first url
    init();
    
    // get normalize of the first url(url).
    if(SetUrl(g_url, g_input) < 0) 
    {
        puts("input url error");
        exit(-1);
    }
    
    // get host by name(do only once in the whole program)
    if(GetHostByName(g_url.GetHost()) < 0)
     {
        printf("gethostbyname error\n");
        exit(1);
    }
    
    // use hash value for html page
    string url_full = g_url.GetHost() + g_url.GetFile();
    unsigned int hashVal = hash(url_full.c_str());
    char tmp[31];
    
    sprintf(tmp, "%010u", hashVal);
    g_url.SetFname(string(tmp));

#ifdef DEBUG
    char info[120];
    sprintf(info, "Add queue: %u %s", hashVal, url_full.c_str());
    PRINT(info);
#endif
    g_que.push(g_url);
        
    pthread_mutex_lock(&setlock);
    g_Set.insert(hashVal);
    pthread_mutex_unlock(&setlock);
    
    // 3. start
    struct timeval t_st, t_ed; // store time value. used to calculate avg-speed.

    gettimeofday(&t_st, NULL);
    start_run();
    gettimeofday(&t_ed, NULL);

    // 4. print summery
    summery(t_st, t_ed);

    return 0;
}

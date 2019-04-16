#include "Aria.h"
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>


#define HTTP_HEADERS_MAXLEN 	512 	// Headers 的最大长度

/*
 * Headers 按需更改
 */
const char *HttpsPostHeaders = 	"User-Agent: Mozilla/4.0 (compatible; MSIE 5.01; Windows NT 5.0)\r\n"
								"Cache-Control: no-cache\r\n"
								"Accept: */*\r\n"
								"Content-type: application/json\r\n";

/*
 * @Name 			- 创建TCP连接, 并建立到连接
 * @Parame *server 	- 字符串, 要连接的服务器地址, 可以为域名, 也可以为IP地址
 * @Parame 	port 	- 端口
 *
 * @return 			- 返回对应sock操作句柄, 用于控制后续通信
 */
int client_connect_tcp(char *server,int port)
{
	int sockfd;
	struct hostent *host;
	struct sockaddr_in cliaddr;

	sockfd=socket(AF_INET,SOCK_STREAM,0);
	if(sockfd < 0){
		perror("create socket error");
		return -1;
	}

	if(!(host=gethostbyname(server))){
		printf("gethostbyname(%s) error!\n", server);
		return -2;
	}

	bzero(&cliaddr,sizeof(struct sockaddr));
	cliaddr.sin_family=AF_INET;
	cliaddr.sin_port=htons(port);
	cliaddr.sin_addr=*((struct in_addr *)host->h_addr);

	if(connect(sockfd,(struct sockaddr *)&cliaddr,sizeof(struct sockaddr))<0){
		perror("[-] error");
		return -3;
	}

	return(sockfd);
}

/*
 * @Name 			- 封装post数据包括headers
 * @parame *host 	- 主机地址, 域名
 * @parame  port 	- 端口号
 * @parame 	page 	- url相对路径
 * @parame 	len 	- 数据内容的长度
 * @parame 	content - 数据内容
 * @parame 	data 	- 得到封装的数据结果
 *
 * @return 	int 	- 返回封装得到的数据长度
 */
int post_pack(const char *host, int port, const char *page, int len, const char *content, char *data)
{
	int re_len = strlen(page) + strlen(host) + strlen(HttpsPostHeaders) + len + HTTP_HEADERS_MAXLEN;

	char *post = NULL;
	post = (char *)malloc(re_len);
	if(post == NULL){
		return -1;
	}

	sprintf(post, "POST %s HTTP/1.0\r\n", page);
	sprintf(post, "%sHost: %s:%d\r\n",post, host, port);
	sprintf(post, "%s%s", post, HttpsPostHeaders);
	sprintf(post, "%sContent-Length: %d\r\n\r\n", post, len);
	sprintf(post, "%s%s", post, content); 		// 此处需要修改, 当业务需要上传非字符串数据的时候, 会造成数据传输丢失或失败

	re_len = strlen(post);
	memset(data, 0, re_len+1);
	memcpy(data, post, re_len);

	free(post);
	return re_len;
}

/*
 * @Name 		- 	初始化SSL, 并且绑定sockfd到SSL
 * 					此作用主要目的是通过SSL来操作sock
 *
 * @return 		- 	返回已完成初始化并绑定对应sockfd的SSL指针
 */
SSL *ssl_init(int sockfd)
{
	int re = 0;
	SSL *ssl;
	SSL_CTX *ctx;

	SSL_library_init();
	SSL_load_error_strings();
	ctx = SSL_CTX_new(SSLv23_client_method());
	if (ctx == NULL){
		return NULL;
	}

	ssl = SSL_new(ctx);
	if (ssl == NULL){
		return NULL;
	}

	/* 把socket和SSL关联 */
	re = SSL_set_fd(ssl, sockfd);
	if (re == 0){
		SSL_free(ssl);
		return NULL;
	}

    /*
     * 经查阅, WIN32的系统下, 不能很有效的产生随机数, 此处增加随机数种子
     */
	RAND_poll();
	while (RAND_status() == 0)
	{
		unsigned short rand_ret = rand() % 65536;
		RAND_seed(&rand_ret, sizeof(rand_ret));
	}

	/*
     * ctx使用完成, 进行释放
     */
	SSL_CTX_free(ctx);

	return ssl;
}

/*
 * @Name 			- 通过SSL建立连接并发送数据
 * @Parame 	*ssl 	- SSL指针, 已经完成初始化并绑定了对应sock句柄的SSL指针
 * @Parame 	*data 	- 准备发送数据的指针地址
 * @Parame 	 size 	- 准备发送的数据长度
 *
 * @return 			- 返回发送完成的数据长度, 如果发送失败, 返回 -1
 */
int ssl_send(SSL *ssl, const char *data, int size)
{
	int re = 0;
	int count = 0;

	re = SSL_connect(ssl);

	if(re != 1){
		return -1;
	}

	while(count < size)
	{
		re = SSL_write(ssl, data+count, size-count);
		if(re == -1){
			return -2;
		}
		count += re;
	}

	return count;
}

/*
 * @Name 			- SSL接收数据, 需要已经建立连接
 * @Parame 	*ssl 	- SSL指针, 已经完成初始化并绑定了对应sock句柄的SSL指针
 * @Parame  *buff 	- 接收数据的缓冲区, 非空指针
 * @Parame 	 size 	- 准备接收的数据长度
 *
 * @return 			- 返回接收到的数据长度, 如果接收失败, 返回值 <0
 */
int ssl_recv(SSL *ssl, char *buff, int size)
{
	int i = 0; 				// 读取数据取换行数量, 即判断headers是否结束
	int re;
	int len = 0;
	char headers[HTTP_HEADERS_MAXLEN];

	if(ssl == NULL){
		return -1;
	}

	// Headers以换行结束, 此处判断头是否传输完成
	while((len = SSL_read(ssl, headers, 1)) == 1)
	{
		if(i < 4){
			if(headers[0] == '\r' || headers[0] == '\n'){
				i++;
				if(i>=4){
					break;
				}
			}else{
				i = 0;
			}
		}
		//printf("%c", headers[0]);		// 打印Headers
	}

	len = SSL_read(ssl, buff, size);
	return len;
}

int https_post(char *host, int port, char *url, const char *data, int dsize, char *buff, int bsize)
{
	SSL *ssl;
	int re = 0;
	int sockfd;
	int data_len = 0;
	int ssize = dsize + HTTP_HEADERS_MAXLEN; 	// 欲发送的数据包大小
	char *sdata =(char*) malloc(ssize);
	if(sdata == NULL){
		return -1;
	}

	// 1、建立TCP连接
	sockfd = client_connect_tcp(host, port);
	if(sockfd < 0){
		free(sdata);
		return -2;
	}

	// 2、SSL初始化, 关联Socket到SSL
	ssl = ssl_init(sockfd);
	if(ssl == NULL){
		free(sdata);
		close(sockfd);
		return -3;
	}

	// 3、组合POST数据
	data_len = post_pack(host, port, url, dsize, data, sdata);

	// 4、通过SSL发送数据
	re = ssl_send(ssl, sdata, data_len);
	if(re < 0){
		free(sdata);
		close(sockfd);
		SSL_shutdown(ssl);
		return -4;
	}

	// 5、取回数据
	int r_len = 0;
	r_len = ssl_recv(ssl, buff, bsize);
	if(r_len < 0){
		free(sdata);
		close(sockfd);
		SSL_shutdown(ssl);
		return -5;
	}

	// 6、关闭会话, 释放内存
	free(sdata);
	close(sockfd);
	SSL_shutdown(ssl);
	ERR_free_strings();
	return r_len;
}

int Port = 5000;
char *Host = "10.0.126.100";
char *Page = "/command";
char *Data = "{\"A\":\"111\", \"B\":\"222\"}"; 	// 对应字符串 - {"A":"111", "B":"222"}
ArRobot robot;
ArSonarDevice sonar;

int main(int argc, char** argv)
{

	/*Robot set up*/
    	Aria::init();
   	   ArArgumentParser parser(&argc, argv);
    	// Set IP
    	parser.addDefaultArgument("-rh 10.0.126.17");

    	ArRobotConnector robotConnector(&parser, &robot);
    	ArLaserConnector laserConnector(&parser, &robot, &robotConnector);

    	if (!robotConnector.connectRobot()) {
       	 	if (!parser.checkHelpAndWarnUnparsed()) {
            	ArLog::log(ArLog::Terse, "Could not connect to robot,");
        	} else {
            	ArLog::log(ArLog::Terse, "Error, could not connect to robot.");
            	Aria::logOptions();
            	Aria::exit(1);
        	}
   	 }

    	if(!robot.isConnected()) {
        	ArLog::log(ArLog::Terse, "Internal error!");
    	}


    	ArCompassConnector compassConnector(&parser);
    	if (!Aria::parseArgs() || !parser.checkHelpAndWarnUnparsed()) {
        	Aria::logOptions();
        	Aria::exit(1);
        	return 1;
    	}
    	robot.addRangeDevice(&sonar);
    	robot.runAsync(true);
	robot.lock();
        
        robot.clearDirectMotion();
        robot.enableMotors();
        robot.unlock();
    	/*Robot set up finish*/

	int read_len = 0;
	char buff[512] = {0};
	char oldmovement = '9';
	char movement;
	while(1){
        read_len = https_post(Host, Port, Page, Data, strlen(Data), buff, 512);

        if(read_len < 0){
            printf("Err = %d \n", read_len);
            return read_len;
        }
	
	if(buff[0]=='2'){
	printf("%s\n",buff);
 	movement =buff[2];
	if(oldmovement!=movement){
	  switch(movement) {
            	case '3'://forward
                	robot.setRotVel(0);
                	robot.setVel(100);
               		 break;
            	case '2':
                	robot.setRotVel(0);
                	robot.setVel(-100);
                	break;
            	case '0'://turn left
                	robot.setVel(0);
                	robot.setRotVel(30);
                	break;
            	case '1':
                	robot.setVel(0);
                	robot.setRotVel(-30);
                	break;
            	case '9':
                	robot.setVel(0);
                	robot.setRotVel(0);
                	break;
            }
	}
        	oldmovement=movement;
	}
        usleep(4000);
    }


	return 1;
}


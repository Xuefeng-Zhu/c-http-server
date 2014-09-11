/**
 * @file server.c
 * Author: Xuefeng Zhu 
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <queue.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "queue.h"
#include "libhttp.h"
#include "libdictionary.h"

const char *HTTP_404_CONTENT = "<html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1>The requested resource could not be found but may be available again in the future.<div style=\"color: #eeeeee; font-size: 8pt;\">Actually, it probably won't ever be available unless this is showing up because of a bug in your program. :(</div></html>";
const char *HTTP_501_CONTENT = "<html><head><title>501 Not Implemented</title></head><body><h1>501 Not Implemented</h1>The server either does not recognise the request method, or it lacks the ability to fulfill the request.</body></html>";

const char *HTTP_200_STRING = "OK";
const char *HTTP_404_STRING = "Not Found";
const char *HTTP_501_STRING = "Not Implemented";

int sock;
queue_t thread_q;
queue_t socket_q;
/**
 * Processes the request line of the HTTP header.
 * 
 * @param request The request line of the HTTP header.  This should be
 *                the first line of an HTTP request header and must
 *                NOT include the HTTP line terminator ("\r\n").
 *
 * @return The filename of the requested document or NULL if the
 *         request is not supported by the server.  If a filename
 *         is returned, the string must be free'd by a call to free().
 */
char* process_http_header_request(const char *request)
{
	// Ensure our request type is correct...
	if (strncmp(request, "GET ", 4) != 0)
		return NULL;

	// Ensure the function was called properly...
	assert( strstr(request, "\r") == NULL );
	assert( strstr(request, "\n") == NULL );

	// Find the length, minus "GET "(4) and " HTTP/1.1"(9)...
	int len = strlen(request) - 4 - 9;

	// Copy the filename portion to our new string...
	char *filename = malloc(len + 1);
	strncpy(filename, request + 4, len);
	filename[len] = '\0';

	// Prevent a directory attack...
	//  (You don't want someone to go to http://server:1234/../server.c to view your source code.)
	if (strstr(filename, ".."))
	{
		free(filename);
		return NULL;
	}

	return filename;
}

void* request_handler(void* fd_value)
{
	int fd = *((int*)fd_value);

	http_t http; 
	while(1)
	{
		int requestL = http_read(&http, fd);
		if (requestL == -1)
		{
			shutdown(fd, SHUT_RDWR);
			http_free(&http);
			break;	
		}
		const char* requestH = http_get_status(&http);
		char* filename = process_http_header_request(requestH);

		void* response;
		char* connection;
		int response_length;
		int response_code;
		const char *response_code_string;
		char* content_type = "text/html";
		int content_length;
		char* content;

		const char* temp = http_get_header(&http, "Connection");
		if (temp == NULL || strcasecmp(temp, "Keep-Alive") != 0)
		{
			connection = "close";	
		}
		else
		{
			connection = "Keep-Alive";	
		}

		if (filename == NULL)
		{
			response_code = 501;	
			response_code_string = HTTP_501_STRING;	
			content = HTTP_501_CONTENT;
			content_length = strlen(HTTP_501_CONTENT);
		}
		else 
		{
			if (strcmp(filename, "/") == 0)
			{
				free(filename);
				filename = malloc(20);
				strcpy(filename, "/index.html");	
			}
			
			int temp = strlen(filename);
			char* filename2 = malloc(temp + 10);
			strcpy(filename2, "web");
			strcat(filename2, filename);
			
			FILE *pfile = fopen(filename2, "rb");
			free(filename2);
			if (pfile == NULL)
			{
				response_code = 404;	
				response_code_string = HTTP_404_STRING;	
				content = HTTP_404_CONTENT; 
				content_length = strlen(HTTP_404_CONTENT);
			}
			else
			{
				response_code = 200;
				response_code_string = HTTP_200_STRING;	

				if (strstr(filename, ".html"))
				{
					content_type = "text/html";	
				}
				else if (strstr(filename, ".css"))
				{
					content_type = "text/css";	
				}
				else if (strstr(filename, ".jpg"))
				{
					content_type = "image/jpeg";
				}
				else if (strstr(filename, ".png"))
				{
					content_type = "image/png";	
				}
				else 
				{
					content_type = "text/plain";	
				}

				fseek(pfile, 0, SEEK_END);
				content_length = ftell(pfile);
				rewind(pfile);
				
				content = malloc(content_length);
				fread(content, 1, content_length, pfile);
				fclose(pfile);
			}
		}
		
		response = malloc(content_length + 400);

		sprintf(response, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n\r\n", response_code, response_code_string,content_type, content_length, connection);

		response_length = strlen((char*)response) + content_length;
		memcpy(response + strlen((char*)response), content, content_length);

		int res = send(fd, response, response_length, 0);

		free(response);
		if (response_code != 501)
		{
			free(filename);
		}
		if (response_code == 200)
		{
			free(content);	
		}
		http_free(&http);

		if (res <=0 || strcasecmp(connection, "Keep-Alive") != 0)
		{
			shutdown(fd, SHUT_RDWR);
			break;	
		}
	}
	return NULL;
}

void sigHandle()
{
	while(queue_size(&socket_q))
	{
		int* fd = queue_dequeue(&socket_q);
		shutdown(*fd, SHUT_RDWR);
		free(fd);
	}
	queue_destroy(&socket_q);

	while(queue_size(&thread_q))
	{
		pthread_t* t = queue_dequeue(&thread_q);
		pthread_join(*t, NULL);
		free(t);
	}
	queue_destroy(&thread_q);

	close(sock);
	exit(0);

}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s [port number]\n", argv[0]);
		return 1;
	}

	int port = atoi(argv[1]);
	if (port <= 0 || port >= 65536)
	{
		fprintf(stderr, "Illegal port number.\n");
		return 1;
	}

	signal(SIGINT, sigHandle);
	sock = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(port);

	if (bind(sock, (struct sockaddr*) &server, sizeof(server)) < 0)
	{
		perror("ERROR BINDING SOCKET");
		exit(1);
	}

	if (listen(sock, 10) < 0)
	{
		perror("ERROR IN LISTEN");
		exit(1);
	}
	
	queue_init(&thread_q);
	queue_init(&socket_q);
	while(1)
	{
		struct sockaddr_in client;	
		socklen_t clientlen = sizeof(client);
		int fd = accept(sock, (struct sockaddr*)&client, &clientlen);
		int* fd_value = malloc(sizeof(int));
		*fd_value = fd;
		queue_enqueue(&socket_q, fd_value);
		pthread_t* t = malloc(sizeof(pthread_t));
		queue_enqueue(&thread_q, t);
		pthread_create(t, NULL, request_handler, (void*)fd_value);
	}

	return 0;
}

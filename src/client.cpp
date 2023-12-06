#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <iostream>
#include <string>
#include <sstream>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h> 
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <netdb.h>
#include <fcntl.h> 
#include "./utils/base64.h"

using namespace std;

#define BUFF_SIZE 1024
#define PORT 9999
#define ERR_EXIT(a){ perror(a); exit(1); }
string msg_content_type = "Content-Type: ";
string mime_text = "text/plain\r\n\r\n";
string mime_video = "video/mp4\r\n\r\n";
string msg_auth ="Authorization: Basic ";
string msg_length ="Content-Length: ";
string user_agent = "User-Agent: CN2021Client/1.0\r\n";
string connection = "Connection: keep-alive\r\n";
string Host = "";
enum FILE_TYPE{
    PLAINFILE,
    VIDEO,
};

struct {
    int sockfd;
    string body;
    int connection = 0;
    unsigned long long body_progress = 0;
    unsigned long long body_size = 0;
} client;


// server will send close connection to client 
// see https://serverfault.com/questions/790197/what-does-connection-close-mean-when-used-in-the-response-message

//https://stackoverflow.com/questions/154536/encode-decode-urls-in-c
string urlEncode(const string& org_string){
    string new_str = "";
    const char* chars = org_string.c_str();
    char bufHex[10];
    int len = strlen(chars);
    for(int i = 0; i < len; i++){
        char org_char = chars[i];
        int int_char = org_char;
        if (isalpha(org_char) || isalnum(org_char) || org_char == '-' || org_char == '_' || org_char == '.' || org_char == '~') new_str += org_char;
        else {
            sprintf(bufHex,"%X",org_char);
            if(int_char < 16) 
                new_str += "%0"; 
            else
                new_str += "%";
            new_str += bufHex;
        }
    }
    return new_str;
 }

void handle_get_request(string& filename,string &Host,int sockfd){
    string method = "GET /api/file/" + filename +" HTTP/1.1\r\n";
    string msg = method + Host + user_agent + connection + "\r\n";
    size_t msg_size = msg.size();
    size_t bytesleft = msg.size();
    ssize_t progress = 0;
    ssize_t count = 0;
    while(progress < msg_size){
        count = write(sockfd, msg.c_str() + progress, bytesleft); // 分斷送
        progress += count;
        bytesleft -= count;
    }
    return;
}

void handle_put_response(int sockfd){
    char buf[4096];
    bzero(buf, 4096);
    int buf_size = read(sockfd, buf, 4096);
    string response_data = string(buf, buf_size);
    string header = response_data.substr(0, response_data.find("\r\n\r\n") + 4);
    size_t first_pos = 0;
    size_t last_pos = 0;
    while((last_pos = header.find("\r\n",first_pos)) != -1){
        if(first_pos != last_pos){                                                                              
            //cout<< first_pos << " " << last_pos << "\n";
            string subtmp = header.substr(first_pos, last_pos - first_pos );
            string key = subtmp.substr(0, subtmp.find(": "));
            string value = subtmp.substr(subtmp.find(": ") + 2);
            if(!key.compare("Connection")){
                if(value == ("keep-alive") || value == ("Keep-alive")){
                    client.connection =1;
                }
                else{
                    client.connection =0;
                }
            }
            else if(key == "Content-Length"){
                client.body_size = atoi(value.c_str());
            }
        }
        first_pos = last_pos+2;
    }
    return;
}

int handle_get_response(int sockfd, string& filename){
    char buf[4096];
    bzero(buf, 4096);
    int buf_size = read(sockfd, buf, 4096);
    string response_data = string(buf, buf_size);
    string header = response_data.substr(0, response_data.find("\r\n\r\n") + 4);
    size_t first_pos = 0;
    size_t last_pos = header.find("\r\n",first_pos);
    string startLine = header.substr(first_pos, last_pos - first_pos );
    stringstream ss(startLine);
    string Version, StatusCode;
    ss >> Version >> StatusCode;
    while((last_pos = header.find("\r\n",first_pos)) != -1){
        if(first_pos != last_pos){                                                                              
            //cout<< first_pos << " " << last_pos << "\n";
            string subtmp = header.substr(first_pos, last_pos - first_pos );
            string key = subtmp.substr(0, subtmp.find(": "));
            string value = subtmp.substr(subtmp.find(": ") + 2);
            if(!key.compare("Connection")){
                if(value == ("keep-alive") || value == ("Keep-alive")){
                    client.connection =1;
                }
                else{
                    client.connection =0;
                }
            }
            else if(key == "Content-Length"){
                client.body_size = atoi(value.c_str());
            }
        }
        first_pos = last_pos+2;
    }
    if(StatusCode == "404"){
        return -1;
    }
    client.body += response_data.substr(first_pos);
    client.body_progress += client.body.size();
    while(client.body_progress < client.body_size){
        bzero(buf,4090);
        buf_size = read(sockfd, buf, 4096);
        client.body += string(buf, buf_size);
        client.body_progress += buf_size;
    }
    umask(0);
    string pathname = "files/" + filename;
    int store_fd = open(pathname.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0777);
    write(store_fd, client.body.c_str(), client.body_size);
    return 0;
}
int put_data(string& filename,string &Host, int argc, char* encode_msg, int sockfd, FILE_TYPE data_type){
    string method = "";
    if(data_type == PLAINFILE){
        method = "POST /api/file HTTP/1.1\r\n";
    }
    else{
        method = "POST /api/video HTTP/1.1\r\n";
    }
    string boundary = "------WebKitFormBoundaryCN2021\r\n";
    string content_disposition = "Content-Disposition: form-data; name=\"upfile\"; filename=\"" + filename + "\r\n";
    string content_type = "Content-Type: multipart/form-data;boundary=----WebKitFormBoundaryCN2021\r\n\r\n";
    string auth = "";
    if(argc == 4)
        auth = msg_auth + (string)encode_msg + "\r\n";
    string file_type = msg_content_type;
    if(data_type == PLAINFILE){
        file_type += mime_text;
    }
    else{
        file_type += mime_video;
    }
    char buf[512];
    bzero(buf, 512);
    filename.pop_back();
    //cerr<< filename << endl;
    int fd = open(filename.c_str(), O_RDONLY);
    if(fd == -1){
        cerr << "Command failed.\n";
        return -1;    
    }
    ssize_t filesize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0 ,SEEK_SET);
    string body = "";
    ssize_t progress = 0;
    ssize_t count = 0;
    while((count = read(fd, buf, 512))){
        body += string(buf, count);
        progress += count;
        if(progress == filesize)
            break;
    }
    string content_length = msg_length + to_string((int)(2* boundary.size() + content_disposition.size() + file_type.size() + body.size() + 2)) + "\r\n";
    string msg = method + Host + user_agent + connection + auth + content_length + content_type + boundary + content_disposition + file_type + body +  "\r\n------WebKitFormBoundaryCN2021--\r\n";
    size_t msg_size = msg.size();
    size_t bytesleft = msg.size();
    progress = 0;
    while(progress < msg_size){
        count = write(sockfd, msg.c_str() + progress, bytesleft); // 分斷送
        progress += count;
        bytesleft -= count;
    }
    return 0;
}

void init_connect(char **argv){
    struct in_addr **addr_list;
    struct hostent *he = gethostbyname(argv[1]);
    addr_list = (struct in_addr **)he->h_addr_list;
    Host = "Host: " + (string)inet_ntoa(*addr_list[0]) + ":" + (string)argv[2] + "\r\n";
    int sockfd;
    struct sockaddr_in addr;

    // Get socket file descriptor
    if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        ERR_EXIT("socket()");
    }

    // Set server address
    bzero(&addr,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(inet_ntoa(*addr_list[0]));
    addr.sin_port = htons(atoi(argv[2]));

    // Connect to the server
    if(connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0){
        ERR_EXIT("connect()");
    }
    client.sockfd = sockfd;
}


void clean_client(){
    client.body = "";
    client.body_progress = 0;
    client.body_size = 0;
}

int main(int argc , char *argv[]){
    mkdir("files", 0777);
    if(argc < 3 && argc > 4){
        cerr <<"Usage: " << *argv << " [host] [port] [username:password]\n";
    }
    char* encode_msg = NULL;
    if(argc == 4){
        size_t output = 0;
        encode_msg = base64_encode((const unsigned char *)argv[3], strlen(argv[3]), &output);
    }
    init_connect(argv);
    while(true){
        cout << "> ";
        char line[512] = {0}, command[512] = {0}, tmp_filename[512] = {0};
        fgets(line, 512, stdin);
        if(!strcmp(line,"quit\n")){
            cout<< "Bye.\n";
            close(client.sockfd);
            exit(0);
        }
        else{
            int count = sscanf(line, "%s %s", command, tmp_filename);
            if(strcmp(tmp_filename, "")==0){
                if(!strcmp(command, "put"))
                    cerr << "Usage: put [file]\n";
                else if (!strcmp(command, "putv"))
                    cerr << "Usage: putv [file]\n";
                else if (!strcmp(command, "get"))
                    cerr << "Usage: get [file]\n";
                else{
                    cerr << "Command Not Found.\n";
                }
                continue;
            }
            string line_buf = (string)line;
            string filename = line_buf.substr(line_buf.find((string)tmp_filename));
            filename.back() = '\"';
            if(!strcmp(command, "put")){
                int status = put_data(filename, Host, argc, encode_msg, client.sockfd, (FILE_TYPE) PLAINFILE);
                if(status == 0){
                    handle_put_response(client.sockfd);
                    if(client.connection == 0){
                        init_connect(argv);
                    }
                    cout << "Command succeeded.\n";
                }
            }
            else if(!strcmp(command, "putv")){
                int status = put_data(filename, Host, argc, encode_msg, client.sockfd, (FILE_TYPE) VIDEO);
                if(status == 0){
                    handle_put_response(client.sockfd);
                    if(client.connection == 0){
                        init_connect(argv);
                    }
                    cout << "Command succeeded.\n";
                }
            }
            else if(!strcmp(command, "get")){
                filename.pop_back();
                string encoded_filename = urlEncode(filename);
                handle_get_request(encoded_filename, Host, client.sockfd);
                int status = handle_get_response(client.sockfd, filename);
                clean_client();
                if(client.connection == 0){
                    init_connect(argv);
                }
                if(status == 0){
                    cout << "Command succeeded.\n";
                }
                else{
                    cerr << "Command failed.\n";
                }
            }
            else{
                cerr << "Command Not Found.\n";
            }
        }
    }
    return 0;
}




    // // Receive message from server
    // ssize_t n;
    // if((n = read(sockfd, buffer, sizeof(buffer))) < 0){
    //     ERR_EXIT("read()");
    // }

    // printf("%s\n", buffer);

    // close(sockfd);
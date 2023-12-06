#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h> 
#include <poll.h>
#include <fcntl.h> 
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <vector>
#include <dirent.h>
#include <algorithm>
#include <sys/stat.h>
#include "./utils/base64.h"
using namespace std;

string msg_ok = "HTTP/1.1 200 OK\r\n";
string msg_server = "Server: CN2021Server/1.0\r\n";
string msg_content_type = "Content-Type: ";
string mime_html = "text/html\r\n";
string mime_video = "video/mp4\r\n";
string mime_frame = "video/iso.segment\r\n";
string mime_audio = "audio/mp4\r\n";
string mime_mpd = "application/dash+xml\r\n";
string mime_text = "text/plain\r\n";
string fake_fav = "image/x-icon\r\n"; //404
string msg_content_length = "Content-Length: ";
string msg_401 = "HTTP/1.1 401 Unauthorized\r\nServer: CN2021Server/1.0\r\nWWW-Authenticate: Basic realm=\"B10902999\"\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nUnauthorized\n";
string msg_404 = "HTTP/1.1 404 Not Found\r\nServer: CN2021Server/1.0\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nNot Found\n";
string msg_405 = "HTTP/1.1 405 Method Not Allowed\r\nServer: CN2021Server/1.0\r\nAllow: GET\r\nContent-Length: 0\r\n\r\n";
string msg_405_post = "HTTP/1.1 405 Method Not Allowed\r\nServer: CN2021Server/1.0\r\nAllow: POST\r\nContent-Length: 0\r\n\r\n";

#define ERR_EXIT(a){ perror(a); exit(1); }
    

struct {
    int listenfd;   
    struct sockaddr_in addr;
    socklen_t addrlen;
    int maxconn;
} server;

typedef struct{
    string Method;
    string Path;
    unordered_map<string,string> header;
    string body;
    string boundary;
    unsigned long long body_progress = 0;
    unsigned long long body_size = 0;
    string base64_key;
    int connection = 0;
} client;

enum state {
    ACCEPT_CONNECTION,
    HTTP_REQUEST,
    HTTP_RESPONSE
};

struct {
    struct pollfd *polls;
    state *states;
    int size;
} poll_queue;

vector<client> client_connection(105, client{});
unordered_map<string, int> valid_endpoint {
    {"/",1}, {"/upload/file",1}, {"/upload/video", 1}, {"/file/", 1}, {"/video/", 1}, {"/api/file", 1}, {"/api/video", 1}
    };

void add_queue(int fd, short events, state state) {
    poll_queue.polls[poll_queue.size] = {fd, events, 0};
    poll_queue.states[poll_queue.size++] = state;
}

void queue_init() {
    poll_queue.size = 0;
    poll_queue.polls = new struct pollfd[server.maxconn];
    poll_queue.states = new state[server.maxconn];
    add_queue(server.listenfd, POLLIN, ACCEPT_CONNECTION);
}


void remove_queue(int index) {
    poll_queue.polls[index] = poll_queue.polls[--poll_queue.size];
    poll_queue.states[index] = poll_queue.states[poll_queue.size];
}

string urlDecode(string &source) {
    string result;
    char ch;
    int i, hex;
    for (i=0; i < source.length(); i++) {
        if (source[i] == '%') {
            sscanf(source.substr(i+1,2).c_str(), "%x", &hex);
            ch = static_cast<char>(hex);
            result += ch;
            i = i+2;
        } else {
            result+=source[i];
        }
    }
    return (result);
}
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

void accept_connection() {
    struct sockaddr_in  client_addr;
    int client_addr_len = sizeof(client_addr);
    int fd = accept(server.listenfd, (struct sockaddr *)&client_addr, (socklen_t*)&client_addr_len);
    // cerr << "accept a connection from "<<fd<<"\n";
    add_queue(fd, POLLIN, HTTP_REQUEST);
}

void init_server(int argc, char **argv){
    if(argc != 2){
        cerr << "Usage: " << *argv << " [port]\n";
        exit(-1);
    }
    // Get socket file descriptor
    if((server.listenfd = socket(AF_INET , SOCK_STREAM , 0)) < 0){
        ERR_EXIT("socket()");
    } 
    fcntl(server.listenfd, F_SETFL, O_NONBLOCK);

    // Set server address information
    bzero(&server.addr, sizeof(server.addr)); // erase the data
    server.addr.sin_family = AF_INET;
    server.addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server.addr.sin_port = htons(atoi(argv[1]));
    server.addrlen = sizeof(server.addr);
    server.maxconn = getdtablesize();
    // Bind the server file descriptor to the server address
    if(bind(server.listenfd, (struct sockaddr *)&server.addr , sizeof(server.addr)) < 0){
        ERR_EXIT("bind()");
    }
    // Listen on the server file descriptor
    if(listen(server.listenfd , 1000) < 0){
        ERR_EXIT("listen()");
    }
}


void handle_request(int fd){
    char buf[4096];
    bzero(buf, 4096);
    int buf_size = -1;
    // busy waiting 沒關係, 因為可以保證clinet一定會block write
    buf_size = read(fd, buf, 4096);
    if(buf_size == 0){
        // cerr << "disconnect from the client" << "\n";
        close(fd);
        client_connection[fd] = client{};
        return;
    }
    //string request_data = (string) buf; 這個寫錯了. 會把\0 忽略掉, 所以要用下面的這個作法.
    string request_data = string(buf, buf_size);
    

    //cout << "------" << endl;
    //cout <<" buffer size"<< buf_size << endl;
    //cout << request_data << endl;
    if(client_connection[fd].header.size() == 0){
        string header = request_data.substr(0, request_data.find("\r\n\r\n") + 4); // 改成直接方開. 因為你不知道body會不會有 \r\n 所以直接把header 跟body切開
        // cerr << header << endl;
        size_t first_pos = 0;
        size_t last_pos = header.find("\r\n",first_pos);
        string startLine = header.substr(first_pos, last_pos - first_pos );
        // cout << startLine << " ! " << buf_size << endl;
        stringstream ss(startLine);
        string Method, Path, Version;
        ss >> Method >> Path >> Version;
        client_connection[fd].Method = Method;
        client_connection[fd].Path = urlDecode(Path);
        first_pos = last_pos + 2;
        while((last_pos = header.find("\r\n",first_pos)) != -1){
            if(first_pos != last_pos){                                                                              
                //cout<< first_pos << " " << last_pos << "\n";
                string subtmp = header.substr(first_pos, last_pos - first_pos );
                string key = subtmp.substr(0, subtmp.find(": "));
                transform(key.begin(), key.end(), key.begin(), ::tolower);
                string value = subtmp.substr(subtmp.find(": ") + 2);
                if(!key.compare("connection")){
                    transform(value.begin(), value.end(), value.begin(), ::tolower);
                    if(value == ("keep-alive")){
                        client_connection[fd].connection =1;
                    }
                    else{
                        client_connection[fd].connection =0;
                        // cout<<"get close connect"<<"\n";
                    }
                }
                else if(key == "authorization"){
                    client_connection[fd].base64_key = value.substr(value.find(" ")+1);

                }
                else if(key == "content-length"){
                    client_connection[fd].body_size = atoi(value.c_str());
                }
                else if(key == "content-type"){
                    client_connection[fd].boundary = "--" + value.substr(value.find("boundary=") + 9);
                }
                client_connection[fd].header[key] = value;
            }
            first_pos = last_pos+2;
        }
        client_connection[fd].body += request_data.substr(first_pos);
        // client_connection[fd].body_progress += buf_size - first_pos; 還是用下面比較robust方法. 如果一不小心你少加某個bytes 就會進入POLLIN永遠出不來
        client_connection[fd].body_progress += client_connection[fd].body.size();
    }
    else{
        client_connection[fd].body += request_data;
        client_connection[fd].body_progress += buf_size;
        //cout<< request_data.size() <<"\n";
    }
    //cout << client_connection[fd].body_progress << "------------------------------ "<< client_connection[fd].body_size<<endl;
    //fflush(stdout);
    // for(auto &[key, valu] : client_connection[fd].header){
    //     cout<<key<<": "<<valu<<endl;
    // }
    if(client_connection[fd].body_progress < client_connection[fd].body_size){
        add_queue(fd, POLLIN, HTTP_REQUEST);
    }
    else{
        add_queue(fd, POLLOUT, HTTP_RESPONSE);
    }
        
    return;
}

int check_connect(int client_fd){
    if(client_connection[client_fd].connection == 0){
        close(client_fd);
        client_connection[client_fd] = client{};
    }
    return client_connection[client_fd].connection;
}
void write_data(int client_fd, string filename, int mode, string list = ""){
    int file_fd = open(filename.c_str(), O_RDONLY);
    //int file_size = lseek(file_fd, 0, SEEK_END);
    string body;
    char buf[512];
    bzero(buf, 512);
    size_t buf_size = 0;
    while((buf_size = read(file_fd, buf, 512))){
        //(string) buf; 舊的寫法會有漏洞
        body += string(buf,buf_size);  
        bzero(buf, 512);
    }
    close(file_fd);
    if(list.size() > 0){
        if(body.find("<?FILE_LIST?>") != string::npos){
            body.replace(body.find("<?FILE_LIST?>"), 13, list);
        }
        else if(body.find("<?VIDEO_LIST?>") != string::npos){
            body.replace(body.find("<?VIDEO_LIST?>"), 14, list);
        }
        else if(body.find("<?VIDEO_NAME?>") != string::npos){
            body.replace(body.find("<?VIDEO_NAME?>"), 14, list);
            char dash_path[1024];
            bzero(dash_path, 1024);
            sprintf(dash_path,"\"/api/video/%s/dash.mpd\"", list.c_str());
            body.replace(body.find("<?MPD_PATH?>"), 12, dash_path);
        }
        else{
            //cerr << "something go wrong..." << "\n";
            exit(-1);
        } 
    }
    else{
        if(body.find("<?FILE_LIST?>") != string::npos){
            body.replace(body.find("<?FILE_LIST?>"), 13, list);
        }
        else if(body.find("<?VIDEO_LIST?>") != string::npos){
            body.replace(body.find("<?VIDEO_LIST?>"), 14, list);
        }
    }
    string mime_type = "";
    switch (mode)
    {
    case 1:
        mime_type = mime_html;
        break;
    case 2:
        mime_type = mime_mpd;
        break;
    case 3:
        mime_type = mime_frame;
        break;
    default:
        mime_type = mime_text;
        break;
    }
    // cout<< body <<endl;
    string content_length = msg_content_length + to_string(((int)body.size())) + "\r\n\r\n";
    string message = msg_ok + msg_server + msg_content_type + mime_type + content_length + body;
    size_t msg_size = message.size();
    size_t bytesleft = message.size();
    off_t progress = 0;
    size_t count;
    while(progress < msg_size){
        count = write(client_fd, message.c_str() + progress, bytesleft); // 分斷送
        progress += count;
        bytesleft -= count;
    }
}

int authorize(int fd){
    //cerr << "authorizing ... \n";
    ifstream seceret_file("secret");
    string line;
    size_t output;
    int auth_val = 0;
    unsigned char* passwd = base64_decode((const char *)client_connection[fd].base64_key.c_str(), client_connection[fd].base64_key.size(), &output);
    string decode_pass = string((char*)passwd, output);
    //cout<<"decode "<<client_connection[fd].base64_key<<endl;
    while(getline(seceret_file, line)){
        if(decode_pass ==  line)
            {
                auth_val = 1;
                break;
            }
    }
    return auth_val;
}

void handle_upload(int fd){
    int auth_val = authorize(fd);
    if(auth_val){
        //file_fd = open("web/uploadf.html", O_RDONLY);
        if(client_connection[fd].Path == "/upload/file")
            write_data(fd, "web/uploadf.html", 1);
        else
            write_data(fd, "web/uploadv.html", 1);
    }
    else{
        write(fd, msg_401.c_str(), msg_401.size());

    }
    return;
}

void show_file_list(int fd, int mode){
    DIR * dir = NULL;
    struct dirent * entry = NULL;
    if(mode){
        dir = opendir("web/files");
        string list;
        while(entry = readdir(dir)){
            if(entry->d_type == DT_REG){
                char tmpbuf[1024];
                bzero(tmpbuf, 1024);
                string encoded_fname = urlEncode((string)(entry->d_name));
                sprintf(tmpbuf, "<tr><td><a href=\"/api/file/%s\">%s</a></td></tr>\n", encoded_fname.c_str(), entry->d_name);
                list += (string)(tmpbuf);
            }
        }
        write_data(fd, "web/listf.rhtml", 1,list);
    }
        
    else{
        dir = opendir("web/videos");
        string list;
        while(entry = readdir(dir)){
            if(entry->d_type == DT_DIR && (strcmp(entry->d_name,".") != 0) && (strcmp(entry->d_name,"..") != 0) && (strcmp(entry->d_name,"tmp") != 0)){
                char tmpbuf[1024];
                bzero(tmpbuf, 1024);
                string encoded_dir = urlEncode((string)(entry->d_name));
                sprintf(tmpbuf, "<tr><td><a href=\"/video/%s\">%s</a></td></tr>\n", encoded_dir.c_str(), entry->d_name);
                list += (string)(tmpbuf);
            }
        }
        write_data(fd, "web/listv.rhtml", 1,list);
    }
    closedir(dir);
    return;
}

void initValidEndpoint(){
    DIR * dir_file = NULL;
    DIR * dir_video = NULL;
    struct dirent * entry = NULL;
    dir_file = opendir("web/files");
    while(entry = readdir(dir_file)){
        if(entry->d_type == DT_REG){
            string tmp = "/api/file/" + (string) entry->d_name;
            valid_endpoint[tmp] = 1;
        }
    }
    closedir(dir_file);
    dir_video = opendir("web/videos");
    while(entry = readdir(dir_video)){
        if(entry->d_type == DT_DIR && (strcmp(entry->d_name,".") != 0) && (strcmp(entry->d_name,"..") != 0) && (strcmp(entry->d_name,"tmp") != 0)){
            string tmp = "/video/" + (string) entry->d_name;
            valid_endpoint[tmp] = 1;
        }
    }
    closedir(dir_video);
    return;
}

void store_data(string body, int fd, int mode = 0){
    umask(0);
    //cout<< "store data " << body.size() << endl;
    size_t first_pos = 0;
    size_t last_pos = 0;
    // cerr << body << endl;
    string header = body.substr(0, body.find("\r\n\r\n") + 4); //跟前面的道理一樣, 你不知道會不會有\r\n
    //cerr<< header << endl;
    string filename = "";
    // cerr<< header <<endl;
    while((last_pos = header.find("\r\n",first_pos)) != -1){
        if(first_pos != last_pos){
            // cout<< first_pos << " " << last_pos << "\n";
            string subtmp = header.substr(first_pos, last_pos - first_pos );
            string key = subtmp.substr(0, subtmp.find(": "));
            string value = subtmp.substr(subtmp.find(": ") + 2);
            if(!key.compare("Content-Disposition")){
                if(value.find("filename") != string::npos){
                    filename = value.substr(value.find("filename=") + 10);
                    filename.back() = '\0';
                    // cerr << filename << endl;
                    if(mode == 1){
                        //cerr<< filename << endl;
                        string data_type = filename.substr(filename.rfind(".mp4"));
                        data_type.pop_back();
                        //int fdddd =  open("Mr.meeeseek.txt", O_CREAT | O_TRUNC | O_WRONLY);
                        //write(fdddd, data_type.c_str(), data_type.size());
                        //close(fdddd);
                        //cerr<< "data_type yo "<< data_type<<endl;
                        //cerr<< data_type.compare(".mp4\0\0") <<endl;
                        //cerr<<data_type.size() << endl;
                        if(data_type == ".mp4"){
                        }
                        else{
                            //cerr<< "sorry for returne"<<endl;
                            write(fd, msg_405.c_str(), msg_405.size());
                            return;
                        } 
                    }
                    // cout << filename << endl;
                    // fflush(stdout);
                    //int tmpfd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
                }
            }
        }
        first_pos = last_pos+2;
    }
    //cerr<< "this is filename " << filename <<endl;
    //cerr << "mode: " << mode<<endl;
    // cerr<< filename << endl;;

    if(filename.size() > 0)
    {   
        //cout<< "data pos " << first_pos <<endl;
        string file_data = body.substr(first_pos);
        char file_path[512];
        bzero(file_path, 512);
        if(mode == 0){
            sprintf(file_path, "web/files/%s", filename.c_str());
        }
        else{
            sprintf(file_path, "web/videos/tmp/%s", filename.c_str());
        }
        umask(0);
        int tmpfd = open(file_path, O_RDWR | O_CREAT | O_TRUNC, 0777);
        // cout << file_data << endl;
        //cerr << "mode: " << mode<<endl;
        //printf("write the video %s\n", file_path);
        //fflush(stdout);
        write(tmpfd, file_data.c_str(), file_data.size());
        close(tmpfd);
        if(mode == 1){
            pid_t pid;
            //printf("run fork to get dash\n");
            //fflush(stdout);
            string file_folder = filename.substr(0,filename.find(".mp4"));
            string tmp = "/video/" + file_folder;
            valid_endpoint[tmp] = 1;
            if((pid = vfork()) == 0){
                //printf("child to make dash\n");
                //fflush(stdout);
                execl("/bin/sh","/bin/sh","make_dash.sh", file_path, file_folder.c_str(),NULL);
            }
            else{
                string body = "Video Uploaded\n";
                string content_length = msg_content_length + to_string(((int)body.size())) + "\r\n\r\n";
                string msg = msg_ok + msg_server + msg_content_type + mime_text + content_length + body;
                write(fd , msg.c_str(), msg.size());
            }
        }
        else{
            string tmp = "/api/file/" + filename;
            //cerr << tmp <<endl;
            valid_endpoint[tmp] = 1;
            string body = "File Uploaded\n";
            string content_length = msg_content_length + to_string(((int)body.size())) + "\r\n\r\n";
            string msg = msg_ok + msg_server + msg_content_type + mime_text + content_length + body;
            write(fd , msg.c_str(), msg.size());
        }
    }
    return;
}

void handle_post(int fd, int mode = 0){
    // cout << client_connection[fd].body << "\n";
    // int fffd = open("post_data", O_CREAT | O_WRONLY | O_TRUNC);
    // write(fffd, client_connection[fd].body.c_str(), client_connection[fd].body_size);
    // close(fffd);
    //cerr << "mode: " << mode<<endl;
    string::iterator it = client_connection[fd].body.begin();
    string::iterator next_it = search(it + client_connection[fd].boundary.size() + 2,client_connection[fd].body.end(), client_connection[fd].boundary.begin(), client_connection[fd].boundary.end());// 2 for CLRF
    while( next_it != client_connection[fd].body.end()){
        //cout << string(it + client_connection[fd].boundary.size() + 2, next_it - 2); //除掉 \r\n 
        //fflush(stdout);
        // string aaa = string(it + client_connection[fd].boundary.size() + 2, next_it - 2);
        // cout << aaa.size() << endl;
        if(mode == 1){
            //printf("store video \n");
            //fflush(stdout);
            store_data(string(it + client_connection[fd].boundary.size() + 2, next_it - 2), fd, 1);
        }else{
            store_data(string(it + client_connection[fd].boundary.size() + 2, next_it - 2), fd);
        }
        it = next_it;
        next_it = search(it + client_connection[fd].boundary.size() + 2,client_connection[fd].body.end(), client_connection[fd].boundary.begin(), client_connection[fd].boundary.end());
    }
    //fflush(stdout);

}



void handle_response(int fd){
    if(client_connection[fd].Method == "GET"){
        //int file_fd = open(client_connection[fd].Path.c_str(), O_RDONLY);
        int file_fd = -1;
        if(client_connection[fd].Path == "/"){
            //file_fd = open("web/index.html", O_RDONLY);
            write_data(fd, "web/index.html", 1);
        }
        else if(client_connection[fd].Path == "/upload/file" || client_connection[fd].Path == "/upload/video"){
            if(client_connection[fd].base64_key.size() == 0){
                write(fd, msg_401.c_str(), msg_401.size());
            }
            else{
                handle_upload(fd);
            }
        }
        else if(client_connection[fd].Path == "/file/"){
            show_file_list(fd, 1);
        }
        else if(client_connection[fd].Path == "/video/"){
            show_file_list(fd, 0);
        }
        else if(client_connection[fd].Path.find("/video/") != string::npos && client_connection[fd].Path.find("/video/") == 0){
            // if(client_connection[fd].Path.find("/video/") != 0){
            //     write(fd, msg_404.c_str(), msg_404.size());
            // }
            string file_path = client_connection[fd].Path.substr(7);
            //cout<< file_path <<endl;
            //cerr << client_connection[fd].Path <<endl;
            //cerr << valid_endpoint[client_connection[fd].Path] <<endl;
            if(file_path.find("../") != string::npos){
                write(fd, msg_404.c_str(), msg_404.size());
            }
            else if(valid_endpoint[client_connection[fd].Path]){
                write_data(fd, "web/player.rhtml", 1, file_path);
            }
            else{
                int tmpfd = open(client_connection[fd].Path.c_str(), O_RDONLY);
                if(tmpfd == -1)
                    write(fd, msg_404.c_str(), msg_404.size());
                else{
                    close(tmpfd);
                    write_data(fd, "web/player.rhtml", 1, file_path);
                }
            }
        }
        else if(client_connection[fd].Path.find("/api/file/") != string::npos && client_connection[fd].Path.find("/api/file/") == 0){
            // if(client_connection[fd].Path.find("/api/file/") != 0){
            //     write(fd, msg_404.c_str(), msg_404.size());
            // }
            string file_path = client_connection[fd].Path.substr(10);
            string path_name = "web/files/" + file_path;
            int tmpfd = -1;
            if(file_path.size() == 0){
                write(fd, msg_405_post.c_str(), msg_405_post.size());
            }
            else if(file_path.find("../") != string::npos){
                write(fd, msg_404.c_str(), msg_404.size());
            }
            else if((tmpfd = open(path_name.c_str(), O_RDONLY)) == -1){
                write(fd, msg_404.c_str(), msg_404.size());
            }
            else{
                close(tmpfd); //要先關掉? 對ㄟ 如果不存在的路徑你 close(-1) 會出事的
                write_data(fd, path_name, 0);
            }
        }
        else if(client_connection[fd].Path.find("/api/video/") != string::npos && client_connection[fd].Path.find("/api/video/") == 0){
            string file_path = client_connection[fd].Path.substr(11);
            string path_name = "web/videos/" + file_path;
            int tmpfd = -1;
            if(file_path.size() == 0){
                write(fd, msg_405_post.c_str(), msg_405_post.size());
            }
            else if(file_path.find("../") != string::npos){
                write(fd, msg_404.c_str(), msg_404.size());
            }
            else if((tmpfd = open(path_name.c_str(), O_RDONLY)) == -1){
                write(fd, msg_404.c_str(), msg_404.size());
            }
            else{
                close(tmpfd); // 掉到這裡代表保證有file descripter  
                string data_type = file_path.substr(file_path.rfind("."));
                if(data_type == ".mpd"){
                    write_data(fd, path_name, 2);
                }
                else if(data_type == ".m4s"){
                    write_data(fd, path_name, 3);
                }
                else{
                    // cerr << "something go wrong about data type\n";
                    exit(-1);                    
                }
            }
        }
        // else if(client_connection[fd].Path.find("/video/") != string::npos){
        //     string video_name = client_connection[fd].Path.substr(client_connection[fd].Path.find("/video/")+7);
        // }
        else if(client_connection[fd].Path == "/favicon.ico"){
            write(fd, msg_404.c_str(), msg_404.size());
        }
        else{
            write(fd, msg_404.c_str(), msg_404.size());
        }
    }
    else if(client_connection[fd].Method == "POST"){
        if(client_connection[fd].Path == "/api/file"){
            // printf("why ???\n");
            // fflush(stdout);
            int auth_val = authorize(fd);
            if(auth_val){
                handle_post(fd);
            }
            else{
                write(fd, msg_401.c_str(), msg_401.size());
            }
            
        }
        else if(client_connection[fd].Path == "/api/video"){
            //printf("video upload ???\n");
            //fflush(stdout);
            int auth_val = authorize(fd);
            if(auth_val){
                handle_post(fd, 1);
            }
            else{
                 write(fd, msg_401.c_str(), msg_401.size());
            }
        }
        else if(valid_endpoint[client_connection[fd].Path]){
            write(fd, msg_405.c_str(), msg_405.size());
        }
        else if(client_connection[fd].Path.find("../") != string::npos){
            write(fd, msg_404.c_str(), msg_404.size());
        }
        else{
            if(client_connection[fd].Path.find("/api/video/") != string::npos && client_connection[fd].Path.find("/api/video/") == 0){
                string file_path = client_connection[fd].Path.substr(11);
                string path_name = "web/videos/" + file_path;
                int tmpfd = open(path_name.c_str(), O_RDONLY);
                if(tmpfd == -1){
                    write(fd, msg_404.c_str(), msg_404.size());
                }
                else{
                    close(tmpfd);
                    write(fd, msg_405.c_str(), msg_405.size());
                }
            }
            else if(client_connection[fd].Path.find("/api/file/") != string::npos && client_connection[fd].Path.find("/api/file/") == 0){
                string file_path = client_connection[fd].Path.substr(10);
                string path_name = "web/files/" + file_path;
                int tmpfd = open(path_name.c_str(), O_RDONLY);
                if(tmpfd == -1){
                    write(fd, msg_404.c_str(), msg_404.size());
                }
                else{
                    close(tmpfd);
                    write(fd, msg_405.c_str(), msg_405.size());
                }
            }
            else if(client_connection[fd].Path.find("/video/") != string::npos && client_connection[fd].Path.find("/video/") == 0){
                string file_path = client_connection[fd].Path.substr(7);
                string path_name = "web/videos/" + file_path;
                int tmpfd = open(path_name.c_str(), O_RDONLY);
                if(tmpfd == -1){
                    write(fd, msg_404.c_str(), msg_404.size());
                }
                else{
                    close(tmpfd);
                    write(fd, msg_405.c_str(), msg_405.size());
                }
            }
            else{
                write(fd, msg_404.c_str(), msg_404.size());
            }
        }
    }
    // response 處理完 把這些初始化.
    client_connection[fd].header = unordered_map<string,string>{};
    client_connection[fd].body = "";
    client_connection[fd].body_progress = 0;
    client_connection[fd].body_size = 0;
    client_connection[fd].boundary = "";
    if(check_connect(fd))
        add_queue(fd, POLLIN, HTTP_REQUEST);
    return;
}

int main(int argc, char *argv[]){
    mkdir("web/files", 0777);
    mkdir("web/videos", 0777);
    mkdir("web/tmp", 0777);
    init_server(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    queue_init();
    //initValidEndpoint();
    while(true) {
        //cerr << "poll size: " << poll_queue.size << '\n';
        if(poll(poll_queue.polls, poll_queue.size, -1) < 0) perror("polling");
        short revents;
        for(int i = 0; i < poll_queue.size; ++i) if(revents = poll_queue.polls[i].revents) {
            // cerr << revents << "\n";
            // perror("state");
            if(poll_queue.states[i] == ACCEPT_CONNECTION) {
                accept_connection();
                continue;
            }
            if(revents & (POLLHUP | POLLERR | POLLNVAL)) close(poll_queue.polls[i].fd);
            else switch(poll_queue.states[i]) {
                case HTTP_REQUEST:
                    // printf("recevive from request %d\n", poll_queue.polls[i].fd);
                    handle_request(poll_queue.polls[i].fd);
                    break;
                case HTTP_RESPONSE:
                    handle_response(poll_queue.polls[i].fd);
                    break;
            }
            remove_queue(i--);
        }
    }

    // close(connfd);
    // close(server.listenfd);
}


// if(send(connfd, message.c_str(), message.size(), 0) < 0){
//     ERR_EXIT("send()");
// }
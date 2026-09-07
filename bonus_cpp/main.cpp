#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static const size_t HARD_LIMIT = 32u * 1024u * 1024u;
static const auto CLIENT_TIMEOUT = std::chrono::seconds(15);

struct CgiMap { std::string ext, interpreter; };
struct Route {
    std::string path = "/";
    std::vector<std::string> methods{"GET"};
    fs::path root = ".";
    std::string index;
    bool autoindex = false;
    std::optional<std::pair<int, std::string>> redirect;
    std::vector<CgiMap> cgi;
};
struct ServerConfig {
    std::vector<std::pair<std::string, int>> listens;
    std::vector<std::string> names;
    size_t body_limit = 1024 * 1024;
    std::map<int, std::string> error_pages;
    std::vector<Route> routes;
};
struct Request {
    std::string method, target, path, query, version;
    std::map<std::string, std::string> headers;
    std::vector<char> body;
    std::string header(const std::string& name) const {
        std::string k = name;
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        auto it = headers.find(k);
        return it == headers.end() ? "" : it->second;
    }
    bool close_requested() const {
        std::string c = header("connection");
        std::transform(c.begin(), c.end(), c.begin(), ::tolower);
        return c == "close";
    }
};
struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<char> body;
};
struct Client {
    int fd = -1, listener_fd = -1;
    std::vector<char> read_buf, write_buf;
    size_t write_pos = 0;
    bool close_after = false, processing = false;
    Clock::time_point last = Clock::now();
};
struct Listener {
    int fd = -1;
    std::string host;
    int port = 0;
    std::vector<size_t> servers;
};
struct Session { std::string user; unsigned long long visits = 0; Clock::time_point last = Clock::now(); };
struct CgiTask {
    int client_fd = -1;
    size_t server_index = 0;
    pid_t pid = -1;
    std::string in_path, out_path;
    bool keep_alive = true;
    Clock::time_point started = Clock::now();
};

static std::string trim(std::string s) {
    auto not_space = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}
static std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream in(s); std::vector<std::string> out; std::string x;
    while (in >> x) { out.push_back(x); }
    return out;
}
static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }
static bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
static std::string clean_line(const std::string& raw) {
    auto pos = raw.find('#'); return trim(raw.substr(0, pos));
}
static size_t parse_size(std::string v) {
    size_t mult = 1;
    if (!v.empty() && (v.back() == 'k' || v.back() == 'K')) { mult = 1024; v.pop_back(); }
    else if (!v.empty() && (v.back() == 'm' || v.back() == 'M')) { mult = 1024 * 1024; v.pop_back(); }
    return std::stoull(v) * mult;
}

static std::vector<std::vector<std::string>> server_blocks(const std::string& text) {
    std::vector<std::vector<std::string>> blocks; std::vector<std::string> cur;
    std::istringstream in(text); std::string raw; bool active = false; int depth = 0;
    while (std::getline(in, raw)) {
        std::string line = clean_line(raw); if (line.empty()) continue;
        if (!active) {
            if (line != "server {") throw std::runtime_error("expected `server {`");
            active = true; depth = 1; cur.clear(); continue;
        }
        depth += std::count(line.begin(), line.end(), '{');
        depth -= std::count(line.begin(), line.end(), '}');
        if (depth == 0) { blocks.push_back(cur); cur.clear(); active = false; }
        else cur.push_back(line);
        if (depth < 0) throw std::runtime_error("unmatched brace");
    }
    if (active || depth != 0) throw std::runtime_error("unterminated server block");
    return blocks;
}

static Route parse_route(const std::string& path, const std::vector<std::string>& lines) {
    Route r; r.path = path;
    for (auto& line : lines) {
        auto p = split_ws(line); if (p.empty()) continue;
        if (p[0] == "methods") { r.methods.assign(p.begin() + 1, p.end()); for (auto& m : r.methods) for (char& c : m) c = std::toupper(c); }
        else if (p[0] == "root" && p.size() == 2) r.root = p[1];
        else if (p[0] == "index" && p.size() == 2) r.index = p[1];
        else if (p[0] == "autoindex" && p.size() == 2) r.autoindex = p[1] == "on";
        else if (p[0] == "return" && p.size() == 3) r.redirect = {{std::stoi(p[1]), p[2]}};
        else if (p[0] == "cgi_extension" && p.size() == 3) r.cgi.push_back({lower(p[1]), p[2]});
        else throw std::runtime_error("bad location directive: " + line);
    }
    return r;
}

static ServerConfig parse_server(const std::vector<std::string>& lines) {
    ServerConfig s; std::set<std::string> listens;
    for (size_t i = 0; i < lines.size();) {
        std::string line = lines[i];
        if (starts_with(line, "location ") && !line.empty() && line.back() == '{') {
            auto p = split_ws(line); if (p.size() < 3) throw std::runtime_error("bad location");
            std::vector<std::string> nested; int depth = 1; ++i;
            for (; i < lines.size() && depth > 0; ++i) {
                depth += std::count(lines[i].begin(), lines[i].end(), '{');
                depth -= std::count(lines[i].begin(), lines[i].end(), '}');
                if (depth > 0) nested.push_back(lines[i]);
            }
            if (depth) throw std::runtime_error("unterminated location");
            s.routes.push_back(parse_route(p[1], nested)); continue;
        }
        auto p = split_ws(line); if (p.empty()) { ++i; continue; }
        if (p[0] == "listen" && p.size() == 2) {
            if (!listens.insert(p[1]).second) throw std::runtime_error("duplicate listen in one server: " + p[1]);
            auto colon = p[1].rfind(':'); if (colon == std::string::npos) throw std::runtime_error("bad listen");
            s.listens.push_back({p[1].substr(0, colon), std::stoi(p[1].substr(colon + 1))});
        } else if (p[0] == "server_name" && p.size() >= 2) {
            for (size_t j = 1; j < p.size(); ++j) s.names.push_back(lower(p[j]));
        } else if (p[0] == "client_max_body_size" && p.size() == 2) s.body_limit = parse_size(p[1]);
        else if (p[0] == "error_page" && p.size() == 3) s.error_pages[std::stoi(p[1])] = p[2];
        else if (p[0] != "}") throw std::runtime_error("bad server directive: " + line);
        ++i;
    }
    if (s.listens.empty() || s.routes.empty()) throw std::runtime_error("server needs listen and location");
    return s;
}

static std::vector<ServerConfig> load_config(const std::string& path) {
    std::ifstream f(path); if (!f) throw std::runtime_error("cannot read config");
    std::stringstream ss; ss << f.rdbuf(); auto blocks = server_blocks(ss.str());
    std::vector<ServerConfig> out;
    for (size_t i = 0; i < blocks.size(); ++i) {
        try { out.push_back(parse_server(blocks[i])); }
        catch (const std::exception& e) { std::cerr << "config warning: server #" << i + 1 << " skipped: " << e.what() << "\n"; }
    }
    if (out.empty()) throw std::runtime_error("no valid server blocks");
    return out;
}

static bool route_match(const std::string& prefix, const std::string& path) {
    if (prefix == "/") return !path.empty() && path[0] == '/';
    if (path == prefix) return true;
    return starts_with(path, prefix) && path.size() > prefix.size() && path[prefix.size()] == '/';
}
static const Route* route_for(const ServerConfig& s, const std::string& path) {
    const Route* best = nullptr;
    for (auto& r : s.routes) if (route_match(r.path, path) && (!best || r.path.size() > best->path.size())) best = &r;
    return best;
}
static const fs::path* default_root(const ServerConfig& s) {
    for (auto& r : s.routes) { if (r.path == "/") return &r.root; }
    return nullptr;
}
static std::string route_relative(const Route& r, const std::string& p) {
    std::string x = r.path == "/" ? p : (starts_with(p, r.path) ? p.substr(r.path.size()) : "");
    while (!x.empty() && x.front() == '/') { x.erase(x.begin()); }
    return x;
}
static std::string html_escape(std::string s) {
    auto repl=[&](const std::string&a,const std::string&b){size_t p=0;while((p=s.find(a,p))!=std::string::npos){s.replace(p,a.size(),b);p+=b.size();}};
    repl("&","&amp;"); repl("<","&lt;"); repl(">","&gt;"); repl("\"","&quot;"); return s;
}
static std::string reason(int s) {
    switch(s){case 200:return"OK";case 201:return"Created";case 204:return"No Content";case 301:return"Moved Permanently";case 400:return"Bad Request";case 403:return"Forbidden";case 404:return"Not Found";case 405:return"Method Not Allowed";case 413:return"Payload Too Large";case 500:return"Internal Server Error";case 502:return"Bad Gateway";case 504:return"Gateway Timeout";default:return"Unknown";}
}
static std::string mime(const fs::path& p) {
    std::string e = lower(p.extension().string());
    if (e == ".html" || e == ".htm") return "text/html; charset=utf-8";
    if (e == ".css") return "text/css; charset=utf-8";
    if (e == ".js") return "application/javascript; charset=utf-8";
    if (e == ".json") return "application/json; charset=utf-8";
    if (e == ".txt") return "text/plain; charset=utf-8";
    if (e == ".png") return "image/png";
    if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
    if (e == ".svg") return "image/svg+xml";
    return "application/octet-stream";
}
static std::vector<char> read_file(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static bool write_file(const fs::path& p, const std::vector<char>& b) { std::ofstream f(p,std::ios::binary); if(!f)return false; f.write(b.data(),b.size()); return bool(f); }
static std::string sanitize_filename(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/'); auto pos=s.rfind('/'); if(pos!=std::string::npos)s=s.substr(pos+1);
    std::string out; for(unsigned char c:s) out += (std::isalnum(c)||c=='.'||c=='-'||c=='_')?char(c):'_';
    if(out.empty()||out=="."||out=="..") return "";
    return out;
}
static int hexv(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return 10+c-'a';if(c>='A'&&c<='F')return 10+c-'A';return -1;}
static std::optional<std::string> normalize_path(const std::string& in) {
    if(in.empty()||in[0]!='/') return {};
    std::string d;
    for(size_t i=0;i<in.size();){if(in[i]=='%'){if(i+2>=in.size())return{};int a=hexv(in[i+1]),b=hexv(in[i+2]);if(a<0||b<0)return{};d.push_back(char(a*16+b));i+=3;}else d.push_back(in[i++]);}
    std::stringstream ss(d);std::string seg;std::vector<std::string> v;while(std::getline(ss,seg,'/')){if(seg.empty()||seg==".")continue;if(seg=="..")return{};v.push_back(seg);}std::string o="/";for(size_t i=0;i<v.size();++i){if(i)o+="/";o+=v[i];}return o;
}
static fs::path safe_join(const fs::path& root, const std::string& rel) {
    fs::path p=root; fs::path r(rel); for(auto& c:r){auto s=c.string(); if(s==".."||s=="/")throw std::runtime_error("unsafe path"); if(s!=".")p/=c;} return p;
}

static std::optional<size_t> find_bytes(const std::vector<char>& b, size_t start, const std::string& n) {
    auto it=std::search(b.begin()+std::min(start,b.size()),b.end(),n.begin(),n.end()); if(it==b.end())return{};return size_t(it-b.begin());
}
struct ParseOut { int kind=0; Request req; size_t consumed=0; std::string error; }; // 0 need,1 complete,2 error
static ParseOut parse_request(const std::vector<char>& b) {
    ParseOut o; if(b.size()>HARD_LIMIT){o.kind=2;o.error="hard request limit";return o;}
    auto he=find_bytes(b,0,"\r\n\r\n");if(!he)return o; std::string h(b.begin(),b.begin()+*he);std::istringstream in(h);std::string line;
    if(!std::getline(in,line)){o.kind=2;o.error="missing request line";return o;}if(!line.empty()&&line.back()=='\r')line.pop_back();auto p=split_ws(line);if(p.size()!=3||p[2]!="HTTP/1.1"){o.kind=2;o.error="bad request line";return o;}
    o.req.method=p[0];std::transform(o.req.method.begin(),o.req.method.end(),o.req.method.begin(),::toupper);o.req.target=p[1];o.req.version=p[2];auto q=p[1].find('?');o.req.path=p[1].substr(0,q);o.req.query=q==std::string::npos?"":p[1].substr(q+1);
    while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;auto c=line.find(':');if(c==std::string::npos){o.kind=2;o.error="bad header";return o;}std::string k=lower(trim(line.substr(0,c))),v=trim(line.substr(c+1));if(k=="content-length"&&o.req.headers.count(k)&&o.req.headers[k]!=v){o.kind=2;o.error="conflicting content-length";return o;}o.req.headers[k]=v;}
    if(!o.req.headers.count("host")){o.kind=2;o.error="Host required";return o;} size_t bs=*he+4;std::string te=lower(o.req.header("transfer-encoding")),cl=o.req.header("content-length");if(!te.empty()&&!cl.empty()){o.kind=2;o.error="conflicting framing";return o;}
    if(!te.empty()){
        if(te.find("chunked")==std::string::npos){o.kind=2;o.error="unsupported transfer-encoding";return o;} size_t pos=bs;std::vector<char> body;
        for(;;){auto le=find_bytes(b,pos,"\r\n");if(!le)return o;std::string sz(b.begin()+pos,b.begin()+*le);auto semi=sz.find(';');if(semi!=std::string::npos)sz=sz.substr(0,semi);size_t n=0;try{n=std::stoull(trim(sz),nullptr,16);}catch(...){o.kind=2;o.error="bad chunk size";return o;}pos=*le+2;if(n==0){if(b.size()<pos+2)return o;if(std::string(b.begin()+pos,b.begin()+pos+2)=="\r\n"){pos+=2;break;}auto end=find_bytes(b,pos,"\r\n\r\n");if(!end)return o;pos=*end+4;break;}if(body.size()+n>HARD_LIMIT){o.kind=2;o.error="chunked body too large";return o;}if(b.size()<pos+n+2)return o;body.insert(body.end(),b.begin()+pos,b.begin()+pos+n);pos+=n;if(std::string(b.begin()+pos,b.begin()+pos+2)!="\r\n"){o.kind=2;o.error="bad chunk terminator";return o;}pos+=2;}o.req.body=std::move(body);o.consumed=pos;
    }else if(!cl.empty()){size_t n=0;try{n=std::stoull(cl);}catch(...){o.kind=2;o.error="bad content-length";return o;}if(n>HARD_LIMIT){o.kind=2;o.error="body too large";return o;}if(b.size()<bs+n)return o;o.req.body.assign(b.begin()+bs,b.begin()+bs+n);o.consumed=bs+n;}else{o.consumed=bs;}
    o.kind=1;return o;
}

static Response text_response(int status, const std::string& text, const std::string& type="text/plain; charset=utf-8") { Response r;r.status=status;r.body.assign(text.begin(),text.end());r.headers.push_back({"Content-Type",type});return r; }
static std::vector<char> serialize(const Response& r, bool keep) {
    std::ostringstream h;h<<"HTTP/1.1 "<<r.status<<" "<<reason(r.status)<<"\r\n";bool len=false,type=false,conn=false;for(auto&x:r.headers){std::string k=lower(x.first);len|=k=="content-length";type|=k=="content-type";conn|=k=="connection";h<<x.first<<": "<<x.second<<"\r\n";}if(!type)h<<"Content-Type: text/plain; charset=utf-8\r\n";if(!len)h<<"Content-Length: "<<r.body.size()<<"\r\n";if(!conn)h<<"Connection: "<<(keep?"keep-alive":"close")<<"\r\n";h<<"Server: localhost-cpp/0.1\r\n\r\n";std::string hs=h.str();std::vector<char> out(hs.begin(),hs.end());out.insert(out.end(),r.body.begin(),r.body.end());return out;
}

class App {
    int ep=-1; std::vector<ServerConfig> cfg; std::unordered_map<int,Listener> listeners;std::unordered_map<int,Client> clients;std::unordered_map<int,CgiTask> cgis;std::unordered_map<std::string,Session> sessions;unsigned long long seq=0;
public:
    explicit App(std::vector<ServerConfig> c):cfg(std::move(c)){ep=epoll_create1(EPOLL_CLOEXEC);if(ep<0)throw std::runtime_error(strerror(errno));bind_all();}
    ~App(){for(auto&[fd,c]:clients)close(fd);for(auto&[fd,l]:listeners)close(fd);if(ep>=0)close(ep);}
    void run(){std::vector<epoll_event> ev(128);for(;;){int n=epoll_wait(ep,ev.data(),ev.size(),100);if(n<0){if(errno==EINTR)continue;throw std::runtime_error(strerror(errno));}poll_cgi();for(int i=0;i<n;++i){int fd=ev[i].data.fd;uint32_t f=ev[i].events;if(listeners.count(fd)){accept_one(fd);continue;}if(!clients.count(fd))continue;if(f&(EPOLLERR|EPOLLHUP|EPOLLRDHUP)){remove(fd);continue;}if(cgis.count(fd))continue;bool writing=!clients[fd].write_buf.empty();if(writing&&(f&EPOLLOUT))write_one(fd);else if(f&EPOLLIN)read_one(fd);}poll_cgi();expire();}}
private:
    void ctl(int op,int fd,uint32_t events){epoll_event e{};e.events=events;e.data.fd=fd;if(epoll_ctl(ep,op,fd,&e)<0)throw std::runtime_error(strerror(errno));}
    void bind_all(){std::map<std::pair<std::string,int>,std::vector<size_t>> groups;for(size_t i=0;i<cfg.size();++i)for(auto&a:cfg[i].listens)groups[a].push_back(i);for(auto&[a,idx]:groups){int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)continue;int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));sockaddr_in sa{};sa.sin_family=AF_INET;sa.sin_port=htons(a.second);if(inet_pton(AF_INET,a.first.c_str(),&sa.sin_addr)!=1||bind(fd,(sockaddr*)&sa,sizeof(sa))<0||listen(fd,256)<0){std::cerr<<"listener "<<a.first<<":"<<a.second<<" skipped: "<<strerror(errno)<<"\n";close(fd);continue;}ctl(EPOLL_CTL_ADD,fd,EPOLLIN|EPOLLRDHUP);listeners[fd]={fd,a.first,a.second,idx};std::cerr<<"listening on "<<a.first<<":"<<a.second<<"\n";}if(listeners.empty())throw std::runtime_error("no listener bound");}
    void accept_one(int lfd){sockaddr_in sa{};socklen_t sl=sizeof(sa);int fd=accept4(lfd,(sockaddr*)&sa,&sl,SOCK_NONBLOCK|SOCK_CLOEXEC);if(fd<0)return;int one=1;setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));try{ctl(EPOLL_CTL_ADD,fd,EPOLLIN|EPOLLRDHUP);}catch(...){close(fd);return;}Client c; c.fd = fd; c.listener_fd = lfd; clients[fd] = std::move(c);}
    void read_one(int fd){char b[65536];ssize_t n=recv(fd,b,sizeof(b),0);if(n==0){remove(fd);return;}if(n<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK)remove(fd);return;}auto&c=clients[fd];c.read_buf.insert(c.read_buf.end(),b,b+n);c.last=Clock::now();process(fd);}
    void write_one(int fd){auto it=clients.find(fd);if(it==clients.end())return;auto&c=it->second;ssize_t n=send(fd,c.write_buf.data()+c.write_pos,c.write_buf.size()-c.write_pos,MSG_NOSIGNAL);if(n<=0){if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return;remove(fd);return;}c.write_pos+=n;c.last=Clock::now();if(c.write_pos==c.write_buf.size()){bool close_after=c.close_after;bool buffered=!c.read_buf.empty();c.write_buf.clear();c.write_pos=0;c.close_after=false;if(close_after){remove(fd);return;}ctl(EPOLL_CTL_MOD,fd,EPOLLIN|EPOLLRDHUP);if(buffered)process(fd);}}
    size_t default_server(int fd){auto&l=listeners.at(clients.at(fd).listener_fd);return l.servers.front();}
    size_t select_server(int fd,const std::string& host){auto&l=listeners.at(clients.at(fd).listener_fd);std::string h=host;auto c=h.find(':');if(c!=std::string::npos)h=h.substr(0,c);h=lower(trim(h));for(size_t i:l.servers)for(auto&n:cfg[i].names)if(lower(n)==h)return i;return l.servers.front();}
    void process(int fd){auto it=clients.find(fd);if(it==clients.end())return;ParseOut p=parse_request(it->second.read_buf);if(p.kind==0)return;if(p.kind==2){queue(fd,error(cfg[default_server(fd)],400,p.error),false);return;}it->second.read_buf.erase(it->second.read_buf.begin(),it->second.read_buf.begin()+p.consumed);size_t si=select_server(fd,p.req.header("host"));auto&sc=cfg[si];if(p.req.body.size()>sc.body_limit){queue(fd,error(sc,413,"body limit exceeded"),!p.req.close_requested());return;}auto norm=normalize_path(p.req.path);if(!norm){queue(fd,error(sc,400,"invalid path"),false);return;}p.req.path=*norm;auto*r=route_for(sc,*norm);if(!r){queue(fd,error(sc,404,"no route"),!p.req.close_requested());return;}handle(fd,si,sc,*r,p.req);}
    bool allowed(const Route&r,const std::string&m){return std::find(r.methods.begin(),r.methods.end(),m)!=r.methods.end();}
    void handle(int fd,size_t si,const ServerConfig&sc,const Route&r,const Request&q){bool keep=!q.close_requested();if(!allowed(r,q.method)){auto x=error(sc,405,"method not allowed");std::string a;for(size_t i=0;i<r.methods.size();++i){if(i)a+=", ";a+=r.methods[i];}x.headers.push_back({"Allow",a});queue(fd,x,keep);return;}if(r.redirect){Response x;text_response(0,"");x.status=r.redirect->first;x.headers.push_back({"Location",r.redirect->second});queue(fd,x,keep);return;}if(auto spec=find_cgi(r,q.path)){start_cgi(fd,si,*spec,q,keep);return;}if(q.path=="/api/echo"&&q.method=="POST"){Response x;x.status=200;x.body=q.body;x.headers.push_back({"Content-Type",q.header("content-type").empty()?"application/octet-stream":q.header("content-type")});queue(fd,x,keep);return;}if(q.path=="/session"&&q.method=="GET"){queue(fd,session(q),keep);return;}if(r.path=="/uploads"&&q.method=="POST"){queue(fd,upload(sc,r,q),keep);return;}if(q.method=="DELETE"){queue(fd,del(sc,r,q.path),keep);return;}if(q.method=="GET"){queue(fd,get(sc,r,q.path),keep);return;}Response x;x.status=200;x.body=q.body;queue(fd,x,keep);}
    Response error(const ServerConfig&sc,int status,const std::string&msg){auto it=sc.error_pages.find(status);auto*root=default_root(sc);if(it!=sc.error_pages.end()&&root){try{fs::path p=safe_join(*root,it->second.substr(it->second.find_first_not_of('/')));if(fs::is_regular_file(p)){Response r;r.status=status;r.body=read_file(p);r.headers.push_back({"Content-Type",mime(p)});return r;}}catch(...){}}return text_response(status,"<!doctype html><html><body><h1>"+std::to_string(status)+"</h1><p>"+html_escape(msg)+"</p></body></html>","text/html; charset=utf-8");}
    Response get(const ServerConfig&sc,const Route&r,const std::string&p){fs::path t;try{t=safe_join(r.root,route_relative(r,p));}catch(...){return error(sc,403,"unsafe path");}if(fs::is_regular_file(t)){Response x;x.status=200;x.body=read_file(t);x.headers.push_back({"Content-Type",mime(t)});return x;}if(fs::is_directory(t)){if(!r.index.empty()&&fs::is_regular_file(t/r.index)){Response x;x.status=200;x.body=read_file(t/r.index);x.headers.push_back({"Content-Type",mime(t/r.index)});return x;}if(r.autoindex){std::vector<fs::directory_entry> e;for(auto&x:fs::directory_iterator(t))e.push_back(x);std::sort(e.begin(),e.end(),[](auto&a,auto&b){return a.path().filename()<b.path().filename();});std::string h="<!doctype html><html><body><h1>Index</h1><ul>";std::string base=p.back()=='/'?p:p+"/";for(auto&x:e){std::string n=x.path().filename().string();if(!n.empty()&&n[0]=='.')continue;h+="<li><a href=\""+base+html_escape(n)+(x.is_directory()?"/":"")+"\">"+html_escape(n)+(x.is_directory()?"/":"")+"</a></li>";}h+="</ul></body></html>";return text_response(200,h,"text/html; charset=utf-8");}return error(sc,403,"autoindex disabled");}return error(sc,404,"not found");}
    std::optional<std::pair<std::string,std::vector<char>>> multipart(const Request&q){std::string ct=q.header("content-type"),tag="boundary=";auto p=ct.find(tag);if(p==std::string::npos)return{};std::string b=ct.substr(p+tag.size());auto semi=b.find(';');if(semi!=std::string::npos)b=b.substr(0,semi);b=trim(b);if(b.size()>=2&&b.front()=='\"'&&b.back()=='\"')b=b.substr(1,b.size()-2);std::string marker="--"+b;size_t pos=0;for(;;){auto st=find_bytes(q.body,pos,marker);if(!st)return{};size_t s=*st+marker.size();if(s+2<=q.body.size()&&std::string(q.body.begin()+s,q.body.begin()+s+2)=="--")return{};if(s+2>q.body.size())return{};s+=2;auto he=find_bytes(q.body,s,"\r\n\r\n");if(!he)return{};std::string hs(q.body.begin()+s,q.body.begin()+*he);auto fn=hs.find("filename=\"");if(fn!=std::string::npos){fn+=10;auto end=hs.find('\"',fn);if(end==std::string::npos)return{};std::string name=sanitize_filename(hs.substr(fn,end-fn));size_t ds=*he+4;auto de=find_bytes(q.body,ds,"\r\n--"+b);if(!de)return{};return{{name,std::vector<char>(q.body.begin()+ds,q.body.begin()+*de)}};}pos=*he+4;}}
    Response upload(const ServerConfig&sc,const Route&r,const Request&q){std::error_code ec;fs::create_directories(r.root,ec);std::string name=route_relative(r,q.path);std::vector<char> body=q.body;if(starts_with(lower(q.header("content-type")),"multipart/form-data")){auto m=multipart(q);if(!m)return error(sc,400,"bad multipart upload");name=m->first;body=std::move(m->second);}else if(name.empty()){name=q.header("x-filename");if(name.empty())name="payload.bin";}name=sanitize_filename(name);if(name.empty())return error(sc,400,"bad filename");fs::path t=r.root/name;if(!write_file(t,body))return error(sc,500,"write failed");Response x=text_response(201,"uploaded "+name+"\n");x.headers.push_back({"Location",r.path+"/"+name});return x;}
    Response del(const ServerConfig&sc,const Route&r,const std::string&p){std::string rel=route_relative(r,p);if(rel.empty())return error(sc,403,"refuse route root");fs::path t;try{t=safe_join(r.root,rel);}catch(...){return error(sc,403,"unsafe path");}if(!fs::exists(t))return error(sc,404,"not found");if(!fs::is_regular_file(t))return error(sc,403,"not a file");std::error_code ec;fs::remove(t,ec);if(ec)return error(sc,403,"delete failed");Response x;x.status=204;return x;}
    std::string cookie(const std::string&h,const std::string&k){std::stringstream ss(h);std::string p;while(std::getline(ss,p,';')){auto e=p.find('=');if(e!=std::string::npos&&trim(p.substr(0,e))==k)return trim(p.substr(e+1));}return"";}
    std::string qvalue(const std::string&q,const std::string&k){std::stringstream ss(q);std::string p;while(std::getline(ss,p,'&')){auto e=p.find('=');if((e==std::string::npos?p:p.substr(0,e))==k)return e==std::string::npos?"":p.substr(e+1);}return"";}
    Response session(const Request&q){std::string id=cookie(q.header("cookie"),"session_id");if(id.empty()){id=std::to_string(getpid())+"-"+std::to_string(++seq)+"-"+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}std::string user=qvalue(q.query,"user");auto& s=sessions[id];if(!user.empty())s.user=user;if(s.user.empty())s.user="auditor";++s.visits;s.last=Clock::now();std::string body="{\"session_id\":\""+id+"\",\"user\":\""+s.user+"\",\"visits\":"+std::to_string(s.visits)+"}";Response x=text_response(200,body,"application/json; charset=utf-8");x.headers.push_back({"Set-Cookie","session_id="+id+"; Path=/; Max-Age=3600; SameSite=Lax"});return x;}
    struct CSpec{std::string interp;fs::path script;std::string path_info;};
    std::optional<CSpec> find_cgi(const Route&r,const std::string&p){if(r.cgi.empty())return{};std::string rel=route_relative(r,p);std::stringstream ss(rel);std::string seg,script;std::vector<std::string> rest;std::vector<std::string> all;while(std::getline(ss,seg,'/'))if(!seg.empty())all.push_back(seg);for(size_t i=0;i<all.size();++i){if(!script.empty())script+="/";script+=all[i];std::string l=lower(all[i]);for(auto&m:r.cgi)if(l.size()>=m.ext.size()&&l.substr(l.size()-m.ext.size())==m.ext){std::string pi;for(size_t j=i+1;j<all.size();++j)pi+="/"+all[j];return CSpec{m.interpreter,r.root/script,pi};}}return{};}
    void start_cgi(int fd,size_t si,const CSpec&s,const Request&q,bool keep){if(!fs::is_regular_file(s.script)){queue(fd,error(cfg[si],404,"CGI script not found"),keep);return;}std::string base="/tmp/localhost-cpp-cgi-"+std::to_string(getpid())+"-"+std::to_string(fd)+"-"+std::to_string(++seq);std::string in=base+".in",out=base+".out";{std::ofstream f(in,std::ios::binary);f.write(q.body.data(),q.body.size());}pid_t pid=fork();if(pid<0){queue(fd,error(cfg[si],500,"fork failed"),keep);return;}if(pid==0){int inf=open(in.c_str(),O_RDONLY),outf=open(out.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);dup2(inf,STDIN_FILENO);dup2(outf,STDOUT_FILENO);close(inf);close(outf);fs::path abs=fs::absolute(s.script);setenv("REQUEST_METHOD",q.method.c_str(),1);setenv("QUERY_STRING",q.query.c_str(),1);setenv("PATH_INFO",s.path_info.c_str(),1);setenv("CONTENT_LENGTH",std::to_string(q.body.size()).c_str(),1);setenv("CONTENT_TYPE",q.header("content-type").c_str(),1);setenv("HTTP_COOKIE",q.header("cookie").c_str(),1);setenv("HTTP_HOST",q.header("host").c_str(),1);setenv("SCRIPT_FILENAME",abs.c_str(),1);chdir(abs.parent_path().c_str());execl(s.interp.c_str(),s.interp.c_str(),abs.filename().c_str(),(char*)nullptr);_exit(127);}clients[fd].processing=true;ctl(EPOLL_CTL_MOD,fd,EPOLLRDHUP);cgis[fd]={fd,si,pid,in,out,keep,Clock::now()};}
    Response parse_cgi(const std::vector<char>&b){std::string s(b.begin(),b.end());size_t p=s.find("\r\n\r\n"),skip=4;if(p==std::string::npos){p=s.find("\n\n");skip=2;}if(p==std::string::npos)return text_response(502,"bad CGI output");std::string h=s.substr(0,p);std::istringstream in(h);std::string line;Response r;r.status=200;while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();auto c=line.find(':');if(c==std::string::npos)continue;std::string k=trim(line.substr(0,c)),v=trim(line.substr(c+1));if(lower(k)=="status")r.status=std::stoi(v);else r.headers.push_back({k,v});}r.body.assign(s.begin()+p+skip,s.end());return r;}
    void poll_cgi(){std::vector<int> fds;for(auto&[fd,t]:cgis)fds.push_back(fd);for(int fd:fds){auto it=cgis.find(fd);if(it==cgis.end())continue;auto&t=it->second;int st=0;pid_t rc=waitpid(t.pid,&st,WNOHANG);bool timeout=Clock::now()-t.started>std::chrono::seconds(5);if(rc==0&&!timeout)continue;if(timeout&&rc==0){kill(t.pid,SIGKILL);waitpid(t.pid,&st,0);}auto task=t;cgis.erase(it);std::vector<char> out=read_file(task.out_path);unlink(task.in_path.c_str());unlink(task.out_path.c_str());if(!clients.count(fd))continue;clients[fd].processing=false;Response r;if(timeout)r=error(cfg[task.server_index],504,"CGI timeout");else if(rc<0||!WIFEXITED(st)||WEXITSTATUS(st)!=0)r=error(cfg[task.server_index],502,"CGI failed");else r=parse_cgi(out);queue(fd,r,task.keep_alive);}}
    void queue(int fd,const Response&r,bool keep){auto it=clients.find(fd);if(it==clients.end())return;it->second.write_buf=serialize(r,keep);it->second.write_pos=0;it->second.close_after=!keep;it->second.last=Clock::now();try{ctl(EPOLL_CTL_MOD,fd,EPOLLOUT|EPOLLRDHUP);}catch(...){remove(fd);}}
    void expire(){std::vector<int> dead;for(auto&[fd,c]:clients)if(!cgis.count(fd)&&Clock::now()-c.last>CLIENT_TIMEOUT)dead.push_back(fd);for(int fd:dead)remove(fd);for(auto it=sessions.begin();it!=sessions.end();)if(Clock::now()-it->second.last>std::chrono::hours(1))it=sessions.erase(it);else++it;}
    void remove(int fd){auto ci=cgis.find(fd);if(ci!=cgis.end()){kill(ci->second.pid,SIGKILL);waitpid(ci->second.pid,nullptr,0);unlink(ci->second.in_path.c_str());unlink(ci->second.out_path.c_str());cgis.erase(ci);}epoll_ctl(ep,EPOLL_CTL_DEL,fd,nullptr);close(fd);clients.erase(fd);}
};

int main(int argc,char**argv){std::string path="localhost.conf";bool check=false;for(int i=1;i<argc;++i){std::string a=argv[i];if((a=="-c"||a=="--config")&&i+1<argc)path=argv[++i];else if(a=="--check-config")check=true;else if(a=="-h"||a=="--help"){std::cout<<"localhost_cpp [-c FILE] [--check-config]\n";return 0;}else{std::cerr<<"unknown argument: "<<a<<"\n";return 2;}}try{auto cfg=load_config(path);if(check){std::cout<<"configuration OK: "<<cfg.size()<<" valid server block(s)\n";return 0;}App app(std::move(cfg));app.run();}catch(const std::exception&e){std::cerr<<"error: "<<e.what()<<"\n";return 1;}return 0;}

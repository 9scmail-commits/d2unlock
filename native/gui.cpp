#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "resources.hpp"
#include "tiers.hpp"
#include "session.hpp"
#include "gsi.hpp"
#include "pixelfont.hpp"
#include <tlhelp32.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincrypt.h>
namespace Gdiplus{using std::min;using std::max;}
#include <gdiplus.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <memory>
using namespace assets;
namespace G=Gdiplus;
using G::Color;
namespace {
constexpr auto clsname=L"DotaunlockWindowV2";
constexpr UINT Update=WM_APP+1,Done=WM_APP+2;
constexpr UINT_PTR SpinTimer=2,WatchTimer=3,CurtainTimer=5,WelcomeTimer=6,HudTimer=7;
constexpr int W=880,H=420,Art=300,Head=64,Bar=48;
constexpr int Panel=232,LX=8,LY=Head+56,LW=Panel-16,LH=H-8-LY,Row=30,RX=Panel+32,RW=W-32-RX;
constexpr float ViewY=LY,ViewH=LH;
enum Zone{Nothing,Minimize,Close,NavHome,NavTiers,NavSettings,Launch,Folder,Logs,Apply,SearchBox,Scroll,WelcomeLink,WelcomeClose,WelcomeDismiss,TogglePlayer,ToggleClock,HudReset,HeroItem=1000,LevelItem=3000};
enum Font{Small,Body,BodyBold,Brand,Tab,Number,Button,Title,Heading,Display,FontCount};
enum SearchId{SearchControl=100};
const Color c_window(255,7,7,8),c_deep(255,4,4,5),c_surface(255,15,15,17),c_hover(255,26,26,29),c_line(255,33,33,37),
    c_text(255,240,240,240),c_body(255,182,182,186),c_muted(255,122,122,128),c_dim(255,72,72,78),c_red(255,194,60,42),c_green(255,74,140,42),c_ok(255,118,178,86),c_bad(255,222,88,70);
constexpr COLORREF edit_bg=RGB(12,12,14),edit_fg=RGB(232,230,227),edit_hint=RGB(84,89,97);
const wchar_t* tier_names[]={L"Бронза",L"Серебро",L"Золото",L"Платина",L"Мастер",L"Грандмастер"};
HWND window,search;HFONT editfont;HBRUSH editbrush;WNDPROC edit_original;std::unique_ptr<G::Font> fonts[FontCount];
std::unique_ptr<G::Bitmap> banner,mosaic,backdrop;
fs::path root=L"C:\\Dotaunlock",runtime;std::wstring notice=L"Не запущено";
std::mutex status_lock;std::atomic_bool busy=false,cancelled=false,failed=false;std::atomic_int stage=-1;std::thread worker;
bool welcome=false;ULONGLONG welcome_since=0;constexpr ULONGLONG WelcomeDelay=3000;void dismiss_welcome();

int page=0;float scale=1;bool smoke=false,overlay=false,closing=false;HICON icons[2]{};std::atomic<DWORD> attached=0;RECT previous_position{};
std::map<unsigned,tiers::Hero> heroes;std::map<unsigned,unsigned> levels;std::vector<unsigned> hero_order,visible;
// In-game widgets (state/hud.cfg, read live by InventoryClient.dll). Both start switched off.
bool hud_on[2]{};float hud_knob[2]{};unsigned hud_reset=0;ULONGLONG hud_reset_at=0;
unsigned current=0,choice=0;int apply_state=0;float scroll=0,drag_offset=0;bool dragging=false;
struct Hit{int id;G::RectF r;};std::vector<Hit> hits;int hot=Nothing,pressed=Nothing;
int px(int v){return int(v*scale+.5f);}
struct Handle{HANDLE h=nullptr;explicit Handle(HANDLE v=nullptr):h(v){}~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}operator HANDLE()const{return h;}};
void update(std::wstring s,int step=-1){{std::lock_guard g(status_lock);notice=std::move(s);}if(step>=0)stage=step;if(window)PostMessageW(window,Update,0,0);}
std::wstring status(){std::lock_guard g(status_lock);return notice;}
std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned slash=0;for(auto c:s){if(c==L'\\'){++slash;continue;}if(c==L'\"')out.append(slash*2+1,L'\\');else out.append(slash,L'\\');slash=0;out+=c;}out.append(slash*2,L'\\');return out+L'\"';}
void check_cancel(){if(cancelled)throw std::runtime_error("Cancelled");}
DWORD run(const fs::path& exe,const std::vector<std::wstring>& args,bool wait,unsigned timeout=240000) {
    if(exe.parent_path()==runtime){security::plain_path(exe);
        int id=exe.filename()==L"InventoryBridge.exe"?102:exe.filename()==L"CatalogBuilder.exe"?103:0;
        if(!id)throw std::runtime_error("Unknown executable component");
        auto res=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);auto ptr=static_cast<const unsigned char*>(LockResource(LoadResource(nullptr,res)));auto size=SizeofResource(nullptr,res);
        if(!ptr||read(exe)!=Bytes(ptr,ptr+size))throw std::runtime_error("Executable component integrity check failed");
        if(id==102){auto dll=runtime/L"InventoryClient.dll";security::plain_path(dll);auto rr=FindResourceW(nullptr,MAKEINTRESOURCEW(101),RT_RCDATA);auto pp=static_cast<const unsigned char*>(LockResource(LoadResource(nullptr,rr)));auto nn=SizeofResource(nullptr,rr);if(!pp||read(dll)!=Bytes(pp,pp+nn))throw std::runtime_error("Client component integrity check failed");}
    }

    check_cancel();std::wstring cmd=quote(exe.wstring());for(auto& a:args)cmd+=L" "+quote(a);
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};auto logfile=root/L"state/launcher.log";
    Handle output(CreateFileW(logfile.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;si.wShowWindow=SW_HIDE;si.hStdInput=input;si.hStdOutput=output;si.hStdError=output;
    PROCESS_INFORMATION pi{};if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&si,&pi))throw std::runtime_error("Process start failed: "+std::to_string(GetLastError()));
    Handle process(pi.hProcess),thread(pi.hThread);if(!wait)return pi.dwProcessId;
    auto start=GetTickCount64();while(WaitForSingleObject(process,100)!=WAIT_OBJECT_0){check_cancel();if(GetTickCount64()-start>timeout)throw std::runtime_error("Operation timed out");}
    DWORD result=1;GetExitCodeProcess(process,&result);return result;
}
std::vector<DWORD> processes(const fs::path& executable) {
    std::vector<DWORD> result;Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));PROCESSENTRY32W e{};e.dwSize=sizeof(e);
    if(Process32FirstW(snap,&e))do{if(_wcsicmp(e.szExeFile,executable.filename().c_str()))continue;
        Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.th32ProcessID));wchar_t path[32768];DWORD n=32768;
        if(p.h&&QueryFullProcessImageNameW(p,0,path,&n)&&!_wcsicmp(fs::path(std::wstring(path,n)).lexically_normal().c_str(),executable.lexically_normal().c_str()))result.push_back(e.th32ProcessID);
    }while(Process32NextW(snap,&e));
    return result;
}
bool module_ready(DWORD pid) {
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));MODULEENTRY32W e{};e.dwSize=sizeof(e);
    bool api=false,client=false,panorama=false;
    if(Module32FirstW(snap,&e))do{
        if(!_wcsicmp(e.szModule,L"steam_api64.dll"))api=true;
        if(!_wcsicmp(e.szModule,L"client.dll"))client=true;
        if(!_wcsicmp(e.szModule,L"panorama.dll"))panorama=true;
    }while(Module32NextW(snap,&e));
    if(!api||!client||!panorama)return false;
    struct WindowProbe{DWORD pid;HWND hwnd;};WindowProbe probe{pid,nullptr};
    EnumWindows([](HWND hwnd,LPARAM value)->BOOL{auto& p=*reinterpret_cast<WindowProbe*>(value);DWORD id;GetWindowThreadProcessId(hwnd,&id);
        RECT r{};GetClientRect(hwnd,&r);if(id==p.pid&&IsWindowVisible(hwnd)&&!GetWindow(hwnd,GW_OWNER)&&r.right>=640&&r.bottom>=360){p.hwnd=hwnd;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&probe));
    if(!probe.hwnd)return false;DWORD_PTR ignored=0;
    return SendMessageTimeoutW(probe.hwnd,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,500,&ignored)!=0;
}
void wait_closed(const fs::path& exe,unsigned seconds) {
    auto deadline=GetTickCount64()+seconds*1000;while(!processes(exe).empty()){check_cancel();if(GetTickCount64()>deadline)throw std::runtime_error("Application did not close: "+utf8(exe.filename().wstring()));Sleep(250);}
}
void install() {
    security::private_directory(root);security::private_directory(root/L"state");
    struct Payload{int id;const wchar_t* name;const unsigned char* data;DWORD size;};std::vector<Payload> files;
    std::uint64_t hash=14695981039346656037ull;
    for(auto [id,name]:std::vector<std::pair<int,const wchar_t*>>{{101,L"InventoryClient.dll"},{102,L"InventoryBridge.exe"},{103,L"CatalogBuilder.exe"}}){
        auto r=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);if(!r)throw std::runtime_error("Embedded component missing");
        auto data=static_cast<const unsigned char*>(LockResource(LoadResource(nullptr,r)));auto size=SizeofResource(nullptr,r);
        if(!data||!size)throw std::runtime_error("Embedded component invalid");for(DWORD i=0;i<size;++i){hash^=data[i];hash*=1099511628211ull;}files.push_back({id,name,data,size});
    }
    std::wostringstream version;version<<L"app-"<<std::hex<<hash;runtime=root/version.str();fs::create_directories(runtime);
    for(auto& f:files){auto target=runtime/f.name;Bytes expected(f.data,f.data+f.size);if(!fs::exists(target)||read(target)!=expected)write(target,expected);}
    write(runtime/L"installed.marker",{'2'});
}
void write_game_state_config(const fs::path& game){
    auto token_file=root/L"state/gsi.token";std::string token;
    if(fs::exists(token_file)){auto bytes=read(token_file);token.assign(bytes.begin(),bytes.end());}
    if(token.size()!=32){
        unsigned char random[16];HCRYPTPROV provider=0;
        bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT)&&CryptGenRandom(provider,sizeof(random),random);
        if(provider)CryptReleaseContext(provider,0);if(!ok)throw std::runtime_error("Random token failed");
        token.clear();for(auto b:random){char part[3];snprintf(part,sizeof(part),"%02x",b);token+=part;}
        write(token_file,Bytes(token.begin(),token.end()));}
    auto folder=game/L"game/dota/cfg/gamestate_integration";fs::create_directories(folder);auto text=gsi::config(token);
    auto file=folder/gsi::config_name;if(!fs::exists(file)||read(file)!=Bytes(text.begin(),text.end()))write(file,Bytes(text.begin(),text.end()));
}
unsigned wait_signed_in(){
    auto deadline=GetTickCount64()+300000;
    for(;;){check_cancel();try{return session::active_account();}catch(const std::exception&){}
        if(GetTickCount64()>deadline)throw std::runtime_error("Steam sign-in did not complete within 5 minutes");Sleep(500);}
}
void workflow() {
    try {
        update(L"Подготовка...",0);install();auto steamdir=steam(),game=dota();auto steam_exe=steamdir/L"Steam.exe",dota_exe=game/L"game/bin/win64/dota2.exe";
        update(L"Закрываю Dota 2...",0);auto running=processes(dota_exe);
        for(auto pid:running)EnumWindows([](HWND hwnd,LPARAM value)->BOOL{DWORD id;GetWindowThreadProcessId(hwnd,&id);if(id==DWORD(value))PostMessageW(hwnd,WM_CLOSE,0,0);return TRUE;},pid);
        wait_closed(dota_exe,30);
        update(L"Обновляю каталог предметов...",3);auto signature=fingerprint(game/L"game/dota/pak01_dir.vpk");
        if(!fs::exists(runtime/L"catalog.source")||read(runtime/L"catalog.source")!=signature||!fs::exists(runtime/L"heroes.tsv")||!fs::exists(runtime/L"catalog.bin")||!fs::exists(runtime/L"emoticons.bin"))
            if(run(runtime/L"CatalogBuilder.exe",{game.wstring(),runtime.wstring()},true))throw std::runtime_error("Catalogue generation failed; see launcher.log");
        update(L"Перезапускаю Steam...",1);if(!processes(steam_exe).empty()){run(steam_exe,{L"-shutdown"},false);wait_closed(steam_exe,45);}
        update(L"Запускаю Steam...",1);run(steam_exe,{},false);
        update(L"Жду входа в Steam...",1);auto account=wait_signed_in();
        (void)account;
        try{write_game_state_config(game);}catch(const std::exception&){if(cancelled)throw;}
        update(L"Запускаю Dota 2...",1);run(steam_exe,{L"-applaunch",L"570"},false);
        DWORD pid=0,candidate_pid=0;unsigned ready_samples=0;auto deadline=GetTickCount64()+300000;
        update(L"Жду загрузку Dota 2...",2);
        while(!pid){
            check_cancel();if(GetTickCount64()>deadline)throw std::runtime_error("Dota did not finish loading. Check Steam sign-in and game startup.");
            auto found=processes(dota_exe);if(found.size()>1)throw std::runtime_error("Multiple Dota processes");
            if(found.size()==1&&module_ready(found[0])){
                if(candidate_pid!=found[0]){candidate_pid=found[0];ready_samples=0;}
                if(++ready_samples>=20)pid=found[0];
            }else ready_samples=0;
            if(!pid)Sleep(500);
        }

        update(L"Подключаюсь...",4);if(run(runtime/L"InventoryBridge.exe",{L"--pid",std::to_wstring(pid)},true,360000))throw std::runtime_error("Connection failed; see inventory.log");
        update(L"Жду инвентарь...",4);deadline=GetTickCount64()+120000;
        while(GetTickCount64()<deadline){check_cancel();if(processes(dota_exe).empty())throw std::runtime_error("Dota was closed");
            auto path=root/L"state/status.json";if(fs::exists(path)){auto s=decode(read(path));if(s.find("\"pid\":"+std::to_string(pid)+",")!=s.npos&&s.find("virtual_cache_delivered")!=s.npos){attached=pid;update(L"Подключено",5);PostMessageW(window,Done,0,0);return;}}
            Sleep(300);
        }throw std::runtime_error("Inventory did not arrive; see inventory.log");
    }catch(const std::exception& e){if(!cancelled)failed=true;update(cancelled?L"Отменено":L"Ошибка: "+wide(e.what()));}
    PostMessageW(window,Done,0,0);
}

G::RectF R(float x,float y,float w,float h){return G::RectF(x,y,w,h);}
Color alpha(const Color& c,int a){return Color(BYTE(a),c.GetR(),c.GetG(),c.GetB());}
int tier(unsigned n){return n>=30?5:n>=25?4:n>=18?3:n>=12?2:n>=6?1:0;}
float pulse(){return .5f+.5f*std::sin(float(GetTickCount64()%1400)/1400.f*6.2832f);}
void round_rect(G::GraphicsPath& p,G::RectF r,float rad){float d=std::min(rad*2,std::min(r.Width,r.Height));if(d<1){p.AddRectangle(r);return;}
    p.AddArc(r.X,r.Y,d,d,180,90);p.AddArc(r.X+r.Width-d,r.Y,d,d,270,90);p.AddArc(r.X+r.Width-d,r.Y+r.Height-d,d,d,0,90);p.AddArc(r.X,r.Y+r.Height-d,d,d,90,90);p.CloseFigure();}
void rect(G::Graphics& g,G::RectF r,const Color& c){G::SolidBrush b(c);g.FillRectangle(&b,r);}
void fill(G::Graphics& g,G::RectF r,float rad,const Color& c){G::SolidBrush b(c);G::GraphicsPath p;round_rect(p,r,rad);g.FillPath(&b,&p);}
void outline(G::Graphics& g,G::RectF r,float rad,const Color& c,float w=1){r.Inflate(-w/2,-w/2);G::GraphicsPath p;round_rect(p,r,rad-w/2);G::Pen pen(c,w);g.DrawPath(&pen,&p);}
void dot(G::Graphics& g,float cx,float cy,float r,const Color& c){G::SolidBrush b(c);g.FillEllipse(&b,cx-r,cy-r,r*2,r*2);}
void lines(G::Graphics& g,std::initializer_list<G::PointF> points,const Color& c,float w){
    G::Pen p(c,w);p.SetStartCap(G::LineCapRound);p.SetEndCap(G::LineCapRound);p.SetLineJoin(G::LineJoinRound);std::vector<G::PointF> v(points);g.DrawLines(&p,v.data(),int(v.size()));}
void hrule(G::Graphics& g,float x1,float x2,float y){rect(g,R(x1,y,x2-x1,1),c_line);}
enum Align{Left,Center,Right};
void text(G::Graphics& g,const std::wstring& s,G::RectF r,int font,const Color& c,Align a=Left,bool wrap=false){
    G::StringFormat f(G::StringFormat::GenericTypographic());
    f.SetAlignment(a==Left?G::StringAlignmentNear:a==Center?G::StringAlignmentCenter:G::StringAlignmentFar);
    f.SetLineAlignment(wrap?G::StringAlignmentNear:G::StringAlignmentCenter);
    f.SetFormatFlags(wrap?G::StringFormatFlagsLineLimit:G::StringFormatFlagsNoWrap);
    f.SetTrimming(wrap?G::StringTrimmingWord:G::StringTrimmingEllipsisCharacter);
    G::SolidBrush b(c);g.DrawString(s.c_str(),int(s.size()),fonts[font].get(),r,&f,&b);
}
void zone(int id,G::RectF r){hits.push_back({id,r});}
int hit(float x,float y){for(auto i=hits.rbegin();i!=hits.rend();++i)if(x>=i->r.X&&y>=i->r.Y&&x<i->r.X+i->r.Width&&y<i->r.Y+i->r.Height)return i->id;return Nothing;}
void icon_search(G::Graphics& g,float x,float y,const Color& c){G::Pen p(c,1.5f);g.DrawEllipse(&p,x+1.5f,y+1.5f,10.f,10.f);lines(g,{{x+10.3f,y+10.3f},{x+14.5f,y+14.5f}},c,1.5f);}
void icon_play(G::Graphics& g,float cx,float cy,float s,const Color& c){G::SolidBrush b(c);G::PointF p[]={{cx-s*.38f,cy-s*.5f},{cx+s*.5f,cy},{cx-s*.38f,cy+s*.5f}};g.FillPolygon(&b,p,3);}
void icon_gear(G::Graphics& g,float cx,float cy,const Color& c,const Color& hole){
    G::Pen tooth(c,4.2f);for(int i=0;i<8;++i){float a=i*.7854f;g.DrawLine(&tooth,cx+6*std::cos(a),cy+6*std::sin(a),cx+10.5f*std::cos(a),cy+10.5f*std::sin(a));}
    dot(g,cx,cy,7.5f,c);dot(g,cx,cy,3.2f,hole);
}
void glass(G::Graphics& g,G::RectF r,float rad,const Color& tint,int sheen=40,int rim=110){
    rad=std::min({rad,r.Width/2,r.Height/2,3.f});fill(g,r,rad,tint);if(rim<=0)return;
    outline(g,r,rad,Color(BYTE(std::clamp(14+rim/5+sheen/6,0,90)),255,255,255));
}
float ptext(G::Graphics& g,const std::wstring& s,float x,float y,float unit,const Color& c){
    auto state=g.Save();g.SetSmoothingMode(G::SmoothingModeNone);g.SetPixelOffsetMode(G::PixelOffsetModeHalf);G::SolidBrush b(c);const auto& table=pixelfont::glyphs();float cx=x;
    for(auto ch:s){
        if(auto f=table.find(pixelfont::upper(ch));f!=table.end())for(int r=0;r<7;++r){const char* row=f->second[r];
            for(int k=0;k<5;++k)if(row[k]=='#'){bool widen=k==4||(row[k+1]=='.'&&(k==3||row[k+2]=='.'));g.FillRectangle(&b,cx+k*unit,y+r*unit,unit*(widen?2:1),unit);}}
        cx+=pixelfont::Advance*unit;
    }
    g.Restore(state);return pixelfont::width(s,unit);
}
void ptext_in(G::Graphics& g,const std::wstring& s,G::RectF r,float unit,const Color& c,int align=0){
    float w=pixelfont::width(s,unit),x=align==1?r.X+(r.Width-w)/2:align==2?r.X+r.Width-w:r.X;ptext(g,s,std::floor(x),std::floor(r.Y+(r.Height-7*unit)/2),unit,c);
}
void logo(G::Graphics& g,float x,float y,float s){
    auto state=g.Save();g.SetSmoothingMode(G::SmoothingModeAntiAlias);g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
    auto tile=R(x,y,s,s);float rad=std::max(2.f,s*.22f);fill(g,tile,rad,Color(255,10,10,12));if(s>=24)outline(g,tile,rad,Color(255,52,52,58),std::max(1.f,s/64));
    const float u=std::max(1.f,std::floor(s/16)),w=pixelfont::width(L"D2",u),h=7*u,left=std::floor(x+(s-w)/2),top=std::floor(y+(s-h)/2);
    ptext(g,L"D",left,top,u,Color(255,245,245,245));ptext(g,L"2",left+pixelfont::Advance*u,top,u,Color(255,236,78,52));
    g.Restore(state);
}
void brand(G::Graphics& g,float x,float y){float cx=x+ptext(g,L"D2",x,y+2,2,Color(255,236,78,52))+4;ptext(g,L"UNLOCK",cx,y+2,2,Color(255,240,240,240));}
void green_button(G::Graphics& g,G::RectF r,int id,const std::wstring& label,bool play=false){
    Color base=pressed==id?Color(255,58,116,32):hot==id?Color(255,96,176,56):Color(255,78,152,44);
    fill(g,r,2,base);outline(g,r,2,Color(70,255,255,255));Color white(255,255,255,255);
    if(play){float w=14+12+pixelfont::width(label,2),x=std::floor(r.X+(r.Width-w)/2);icon_play(g,x+6,r.Y+r.Height/2,14,white);ptext_in(g,label,R(x+26,r.Y,w,r.Height),2,white);}else ptext_in(g,label,r,2,white,1);
}
Color dark_fill(int id,bool enabled){return !enabled?Color(200,10,10,12):pressed==id?Color(230,6,6,7):hot==id?Color(230,24,24,27):Color(220,14,14,16);}
void dark_button(G::Graphics& g,G::RectF r,int id,const std::wstring& label,int font=Button,bool enabled=true){
    (void)font;glass(g,r,2,dark_fill(id,enabled),0,enabled?(hot==id?150:95):40);ptext_in(g,label,r,2,enabled?c_text:c_dim,1);
}

fs::path art_file(const wchar_t* name){
    try{auto base=steam()/L"appcache/librarycache";
        if(fs::is_directory(base/L"570"))for(auto& e:fs::recursive_directory_iterator(base/L"570",fs::directory_options::skip_permission_denied))if(e.path().filename()==name)return e.path();
        auto flat=base/(std::wstring(L"570_")+name);if(fs::exists(flat))return flat;
    }catch(...){}
    return {};
}
std::unique_ptr<G::Bitmap> load_image(const fs::path& path){
    if(path.empty())return nullptr;auto image=std::make_unique<G::Bitmap>(path.c_str());
    if(image->GetLastStatus()!=G::Ok||!image->GetWidth()||!image->GetHeight())return nullptr;return image;
}
void gradient(G::Graphics& g,G::RectF r,G::PointF from,G::PointF to,const Color& a,const Color& b){G::LinearGradientBrush brush(from,to,a,b);brush.SetWrapMode(G::WrapModeTileFlipXY);g.FillRectangle(&brush,r);}
void build_banner(G::Bitmap* art,G::Bitmap* logo_image){
    banner=std::make_unique<G::Bitmap>(px(W),px(Art),PixelFormat32bppPARGB);G::Graphics g(banner.get());
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);g.SetInterpolationMode(G::InterpolationModeHighQualityBicubic);g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
    g.SetTextRenderingHint(G::TextRenderingHintAntiAliasGridFit);g.ScaleTransform(scale,scale);g.Clear(c_window);
    if(art){float aw=float(art->GetWidth()),ah=float(art->GetHeight()),s=std::max(W/aw,Art/ah);g.DrawImage(art,R((W-aw*s)/2,(Art-ah*s)/2,aw*s,ah*s));}
    else gradient(g,R(0,0,W,Art),G::PointF(0,0),G::PointF(W,Art),Color(255,58,22,17),c_window);
    gradient(g,R(0,0,W,90),G::PointF(0,-1),G::PointF(0,91),Color(160,0,0,0),Color(0,0,0,0));
    gradient(g,R(0,0,560,Art),G::PointF(-1,0),G::PointF(561,0),Color(150,0,0,0),Color(0,0,0,0));
    gradient(g,R(0,Art-160,W,160),G::PointF(0,Art-161),G::PointF(0,Art+1),alpha(c_window,0),c_window);
    if(logo_image){float lw=300,lh=lw*logo_image->GetHeight()/logo_image->GetWidth();g.DrawImage(logo_image,R(38,Art-44-lh,lw,lh));}
    else text(g,L"DOTA 2",R(36,Art-140,W-72,96),Display,c_text);
}
constexpr int MosaicCols=64,MosaicRows=int(Art*MosaicCols/double(W)+.5);
constexpr ULONGLONG CurtainPeriod=5000,CurtainDelay=1500,CurtainStagger=35,CurtainFade=120,CurtainHold=700,
    CurtainIn=(MosaicRows-1)*CurtainStagger+CurtainFade,CurtainLength=2*CurtainIn+CurtainHold;
ULONGLONG curtain_origin=0,curtain_fixed=~0ull;bool curtain_drawn=false;
int strip_edge(int i){return int(std::lround(double(i)*px(Art)/MosaicRows));}
int column_edge(int i){return int(std::lround(double(i)*px(W)/MosaicCols));}
void build_mosaic(){
    if(!banner)return;G::Bitmap small(MosaicCols,MosaicRows,PixelFormat32bppARGB);
    {G::Graphics s(&small);s.SetInterpolationMode(G::InterpolationModeHighQualityBilinear);s.SetPixelOffsetMode(G::PixelOffsetModeHalf);
     s.DrawImage(banner.get(),G::Rect(0,0,MosaicCols,MosaicRows),0,0,px(W),px(Art),G::UnitPixel);}
    mosaic=std::make_unique<G::Bitmap>(px(W),px(Art),PixelFormat32bppPARGB);G::Graphics g(mosaic.get());g.SetSmoothingMode(G::SmoothingModeNone);g.Clear(Color(0,0,0,0));
    auto snap=[](int v,int mean){float lifted=(v-mean)*1.35f+mean+(mean-110)*.25f;return BYTE(std::clamp(int(std::lround(std::clamp(lifted,0.f,255.f)/36.f)*36.f),0,255));};
    for(int r=0;r<MosaicRows;++r)for(int c=0;c<MosaicCols;++c){
        Color color;small.GetPixel(c,r,&color);int mean=(color.GetR()+color.GetG()+color.GetB())/3;Color flat(255,snap(color.GetR(),mean),snap(color.GetG(),mean),snap(color.GetB(),mean));
        int x=column_edge(c),y=strip_edge(r),w=column_edge(c+1)-x,h=strip_edge(r+1)-y;
        G::SolidBrush fill_brush(flat);g.FillRectangle(&fill_brush,x,y,w,h);
    }
}
void build_backdrop(){
    if(!banner)return;G::Bitmap small(24,10,PixelFormat32bppARGB);
    {G::Graphics s(&small);s.SetInterpolationMode(G::InterpolationModeHighQualityBilinear);s.DrawImage(banner.get(),G::Rect(0,0,24,10),0,px(Art)/3,px(W),px(Art)*2/3,G::UnitPixel);}
    backdrop=std::make_unique<G::Bitmap>(px(W),px(H),PixelFormat32bppPARGB);G::Graphics g(backdrop.get());g.Clear(c_window);
    g.SetInterpolationMode(G::InterpolationModeHighQualityBilinear);g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
    G::ImageAttributes clamp;clamp.SetWrapMode(G::WrapModeTileFlipXY);
    g.DrawImage(&small,G::Rect(0,0,px(W),px(H)),0,0,24,10,G::UnitPixel,&clamp);
    G::SolidBrush dim(Color(238,6,6,7));g.FillRectangle(&dim,0,0,px(W),px(H));
}
bool curtain_allowed(){BOOL animate=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animate,0);return mosaic&&animate&&page==0&&window&&!IsIconic(window);}
ULONGLONG curtain_time(){
    if(curtain_fixed!=~0ull)return curtain_fixed;auto now=GetTickCount64();
    if(smoke||!curtain_allowed()||now<curtain_origin)return CurtainLength;return (now-curtain_origin)%CurtainPeriod;
}
float curtain_level(int strip,ULONGLONG t){
    auto ramp=[t](ULONGLONG start){return t<start?0.f:std::min(1.f,float(t-start)/CurtainFade);};
    auto start=strip*CurtainStagger;return ramp(start)-ramp(start+CurtainIn+CurtainHold);
}
void draw_banner(G::Graphics& g,int top,int height){
    if(!banner){rect(g,R(0,0,W,float(height)),c_deep);return;}
    auto state=g.Save();g.ResetTransform();g.SetInterpolationMode(G::InterpolationModeNearestNeighbor);
    g.DrawImage(banner.get(),G::RectF(0,0,float(px(W)),float(px(height))),0,float(px(top)),float(px(W)),float(px(height)),G::UnitPixel);
    if(top==0&&mosaic){auto t=curtain_time();
        if(t<CurtainLength)for(int i=0;i<MosaicRows;++i){auto level=curtain_level(i,t);if(level<=0)continue;
            int y=strip_edge(i),h=std::min(strip_edge(i+1),px(height))-y;if(h<=0)break;
            G::ColorMatrix m={{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,level,0},{0,0,0,0,1}}};G::ImageAttributes fade;fade.SetColorMatrix(&m);
            g.DrawImage(mosaic.get(),G::Rect(0,y,px(W),h),0,y,px(W),h,G::UnitPixel,level<1?&fade:nullptr);}
    }
    g.Restore(state);
}
void curtain_tick(){bool moving=curtain_time()<CurtainLength;if(moving||curtain_drawn){RECT r{0,0,px(W),px(Art)};InvalidateRect(window,&r,FALSE);}curtain_drawn=moving;}

unsigned saved_level(unsigned id){auto i=levels.find(id);return i==levels.end()?0:i->second;}
std::wstring query(){if(!search)return L"";wchar_t b[128]{};GetWindowTextW(search,b,128);std::wstring s(b);CharLowerBuffW(s.data(),DWORD(s.size()));return s;}
float max_scroll(){return std::max(0.f,float(visible.size()*Row)-ViewH);}
void clamp_scroll(){scroll=std::clamp(scroll,0.f,max_scroll());}
float thumb_h(){float c=float(visible.size()*Row);return c<=ViewH?ViewH:std::max(32.f,ViewH*ViewH/c);}
float thumb_y(){auto m=max_scroll();return ViewY+(m>0?(ViewH-thumb_h())*scroll/m:0);}
void repaint(){if(window)InvalidateRect(window,nullptr,FALSE);}
void select_hero(unsigned id){
    if(!heroes.contains(id))return;current=id;choice=saved_level(id);if(choice>heroes.at(id).xp.size())choice=0;apply_state=0;
    auto i=std::find(visible.begin(),visible.end(),id);if(i!=visible.end()){float top=float((i-visible.begin())*Row);if(top<scroll)scroll=top;if(top+Row>scroll+ViewH)scroll=top+Row-ViewH;clamp_scroll();}
    repaint();
}
void filter(){
    auto q=query();visible.clear();
    for(auto id:hero_order){auto n=wide(heroes.at(id).name);CharLowerBuffW(n.data(),DWORD(n.size()));if(q.empty()||n.find(q)!=n.npos)visible.push_back(id);}
    scroll=0;if(!visible.empty()&&std::find(visible.begin(),visible.end(),current)==visible.end())select_hero(visible[0]);else select_hero(current);
}
void apply(){
    if(!heroes.contains(current))return;
    try{if(choice)levels[current]=choice;else levels.erase(current);std::ostringstream out;out<<"OVITIER1\n";for(auto [hero,n]:levels)out<<hero<<' '<<n<<'\n';
        auto data=out.str();write(root/L"state/tiers.txt",Bytes(data.begin(),data.end()));apply_state=1;}catch(...){apply_state=2;}
    repaint();
}
void drag_to(float y){auto range=ViewH-thumb_h();scroll=range>0?(y-drag_offset-ViewY)/range*max_scroll():0;clamp_scroll();repaint();}
void start_drag(float y){auto t=thumb_y(),th=thumb_h();drag_offset=y>=t&&y<t+th?y-t:th/2;dragging=true;drag_to(y);}

void window_buttons(G::Graphics& g){
    auto min=R(W-92,0,46,36),close=R(W-46,0,46,36);zone(Minimize,min);zone(Close,close);
    if(hot==Minimize)rect(g,min,Color(50,255,255,255));if(hot==Close)rect(g,close,Color(255,196,43,28));
    Color c(220,255,255,255);lines(g,{{W-74.f,18},{W-64.f,18}},c,1.2f);lines(g,{{W-28.f,13},{W-18.f,23}},c,1.2f);lines(g,{{W-18.f,13},{W-28.f,23}},c,1.2f);
}
void header(G::Graphics& g,const wchar_t* title){
    draw_banner(g,100,Head);rect(g,R(0,0,W,Head),Color(215,7,7,8));rect(g,R(0,Head-1,W,1),c_line);
    auto back=R(0,0,56,Head);zone(NavHome,back);if(hot==NavHome)glass(g,R(10,12,40,40),20,Color(60,255,255,255),40,120);
    lines(g,{{32,Head/2.f-8},{24,Head/2.f},{32,Head/2.f+8}},Color(255,255,255,255),2);
    ptext_in(g,title,R(62,0,230,Head),2,Color(255,255,255,255));window_buttons(g);
}
void home(G::Graphics& g){
    draw_banner(g,0,Art);
    brand(g,18,11);window_buttons(g);
    if(stage>=0&&stage<5&&(busy||failed)){float seg=W/5.f;auto done=R(0,Art-3,seg*stage,3),part=R(seg*stage,Art-3,seg,3);
        rect(g,done,c_ok);rect(g,part,failed?c_bad:alpha(c_ok,60+int(170*pulse())));}

    auto play=R(40,Art+30,250,60);zone(Launch,play);
    if(busy)dark_button(g,play,Launch,cancelled?L"ОТМЕНЯЮ...":L"ОТМЕНА");else green_button(g,play,Launch,attached?L"ПЕРЕЗАПУСТИТЬ":L"ЗАПУСТИТЬ",true);

    auto gear=R(W-40-60,Art+30,60,60),tiers=R(gear.X-10-170,Art+30,170,60);
    zone(NavSettings,gear);glass(g,gear,12,dark_fill(NavSettings,true),hot==NavSettings?48:34,hot==NavSettings?140:95);icon_gear(g,gear.X+30,gear.Y+30,hot==NavSettings?c_text:c_body,Color(255,24,27,32));
    if(!busy)zone(NavTiers,tiers);dark_button(g,tiers,NavTiers,L"ТИРЫ",Tab,!busy);

    float sx=play.X+play.Width+16,sw=tiers.X-16-sx;auto pill=R(sx,Art+30,sw,60);glass(g,pill,14,Color(70,30,34,41),22,60);
    auto c=attached?c_ok:busy?alpha(c_ok,110+int(145*pulse())):failed?c_bad:c_dim;
    dot(g,sx+20,Art+50,4,c);
    if(failed)text(g,status(),R(sx+34,Art+39,sw-44,44),Body,c_bad,Left,true);
    else{text(g,status(),R(sx+34,Art+38,sw-44,24),BodyBold,c_text);text(g,L"F8 — тиры в игре",R(sx+34,Art+62,sw-44,20),Small,c_muted);}
}
void load_hud(){try{auto file=root/L"state/hud.cfg";if(!fs::exists(file))return;std::istringstream in(decode(read(file)));std::string line;
    while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line=="player=1")hud_on[0]=true;if(line=="clock=1")hud_on[1]=true;if(line.rfind("reset=",0)==0)hud_reset=unsigned(std::strtoul(line.c_str()+6,nullptr,10));}
    for(int i=0;i<2;++i)hud_knob[i]=hud_on[i]?1.f:0.f;}catch(...){}}
void save_hud(){try{std::string text=std::string("player=")+(hud_on[0]?"1":"0")+"\nclock="+(hud_on[1]?"1":"0")+"\nreset="+std::to_string(hud_reset)+"\n";
    write(root/L"state/hud.cfg",Bytes(text.begin(),text.end()));}catch(...){}}
void toggle_switch(G::Graphics& g,G::RectF r,int id,float k){
    auto lerp=[k](BYTE a,BYTE b){return BYTE(a+(b-a)*k);};Color off(255,34,34,38),on=c_green;
    fill(g,r,r.Height/2,Color(255,lerp(off.GetR(),on.GetR()),lerp(off.GetG(),on.GetG()),lerp(off.GetB(),on.GetB())));
    outline(g,r,r.Height/2,Color(BYTE(hot==id?130:70),255,255,255));
    float d=r.Height-8,x=r.X+4+(r.Width-8-d)*k,cy=r.Y+r.Height/2;dot(g,x+d/2,cy+1.5f,d/2,Color(70,0,0,0));dot(g,x+d/2,cy,d/2,Color(255,248,248,248));
}
void settings_page(G::Graphics& g){
    header(g,L"НАСТРОЙКИ");
    const float bx=W-32-160;
    auto section=[&](float y,const wchar_t* title,const std::wstring& value){ptext(g,title,32,y+3,2,c_text);text(g,value,R(32,y+20,bx-48,22),Small,c_muted);};
    section(78,L"ПАПКА",root.wstring());{auto b=R(bx,78,160,36);zone(Folder,b);dark_button(g,b,Folder,L"ОТКРЫТЬ");}hrule(g,32,W-32,132);
    section(140,L"ЛОГИ",L"launcher.log, inventory.log, skins.txt");{auto b=R(bx,140,160,36);zone(Logs,b);dark_button(g,b,Logs,L"ОТКРЫТЬ");}hrule(g,32,W-32,194);
    auto row=[&](float y,int id,const wchar_t* title,const wchar_t* about,int i){
        auto r=R(20,y-6,W-40,56);zone(id,r);if(hot==id)glass(g,r,4,Color(22,255,255,255),0,30);
        section(y,title,about);toggle_switch(g,R(W-32-58,y+6,58,30),id,hud_knob[i]);};
    row(202,TogglePlayer,L"ПЛЕЕР В ИГРЕ",L"Spotify, Яндекс Музыка, браузер: трек, кнопки и прогресс",0);hrule(g,32,W-32,256);
    row(264,ToggleClock,L"ЧАСЫ И FPS",L"Время, дата, FPS и график времени кадра",1);hrule(g,32,W-32,318);
    section(326,L"ВИДЖЕТЫ",L"Тащи мышью, а за край или угол меняй размер");
    {auto b=R(bx,326,160,36);zone(HudReset,b);dark_button(g,b,HudReset,GetTickCount64()-hud_reset_at<1500?L"СБРОШЕНО":L"СБРОСИТЬ");}
    text(g,L"D2UNLOCK 3.8 · F8 — тиры в игре",R(32,380,bx-48,30),Small,c_dim);
}
void tiers_page(G::Graphics& g){
    header(g,L"ТИРЫ ГЕРОЕВ");
    rect(g,R(0,Head,Panel,H-Head),c_deep);rect(g,R(Panel-1,Head,1,H-Head),c_line);
    auto box=R(16,Head+12,Panel-32,32);bool focus=search&&GetFocus()==search;zone(SearchBox,box);
    glass(g,box,8,Color(255,27,30,35),0,focus?140:70);icon_search(g,26,Head+20,focus?c_body:c_muted);
    if(query().empty())text(g,L"Поиск",R(46,Head+12,Panel-72,32),Body,c_dim);

    auto view=R(LX,ViewY,LW,ViewH);bool bar=max_scroll()>0;float iw=LW-(bar?8:0);
    g.SetClip(view);
    for(size_t i=size_t(scroll/Row);i<visible.size();++i){float y=ViewY+i*Row-scroll;if(y>ViewY+ViewH)break;
        auto id=visible[i];bool sel=id==current,hov=hot==HeroItem+int(i);auto r=R(LX,y,iw,Row);
        if(sel){glass(g,r,8,Color(170,40,44,52),34,100);rect(g,R(r.X,r.Y+8,2,r.Height-16),c_red);}else if(hov)glass(g,r,8,Color(60,255,255,255),16,40);
        text(g,wide(heroes.at(id).name),R(r.X+14,r.Y,r.Width-54,r.Height),Body,sel?c_text:c_body);
        if(auto n=saved_level(id))text(g,std::to_wstring(n),R(r.X+r.Width-50,r.Y,40,r.Height),Number,c_muted,Right);
        float top=std::max(r.Y,ViewY),bottom=std::min(r.Y+r.Height,ViewY+ViewH);if(bottom>top)zone(HeroItem+int(i),R(r.X,top,r.Width,bottom-top));
    }
    g.ResetClip();
    if(visible.empty())text(g,heroes.empty()?L"Нет героев":L"Не найдено",R(LX,LY,LW,60),Body,c_muted,Center);
    if(bar){rect(g,R(LX+LW-2,thumb_y(),2,thumb_h()),dragging||hot==Scroll?c_muted:c_line);zone(Scroll,R(LX+LW-8,ViewY,8,ViewH));}

    if(!heroes.contains(current))return;
    auto& hero=heroes.at(current);unsigned top=unsigned(hero.xp.size()),saved=saved_level(current);
    auto name=wide(hero.name);CharUpperBuffW(name.data(),DWORD(name.size()));
    ptext_in(g,name,R(RX,Head+14,RW,46),4,c_text);
    text(g,choice?L"Уровень "+std::to_wstring(choice)+L", "+tier_names[tier(choice)]:std::wstring(L"Уровень из игры"),R(RX,Head+58,RW,22),Body,c_muted);

    const float gap=6,cw=(RW-9*gap)/10,ch=40,y0=Head+96;
    for(unsigned n=1;n<=top;++n){auto r=R(RX+(n-1)%10*(cw+gap),y0+(n-1)/10*(ch+gap),cw,ch);int id=LevelItem+int(n);zone(id,r);
        if(choice==n){glass(g,r,2,Color(255,184,58,40),0,160);ptext_in(g,std::to_wstring(n),r,2,Color(255,255,255,255),1);}
        else{glass(g,r,2,hot==id?Color(255,24,24,27):Color(255,14,14,16),0,saved==n?300:(hot==id?150:80));ptext_in(g,std::to_wstring(n),r,2,hot==id||saved==n?c_text:c_body,1);}
    }

    float fy=H-24-48;std::wstring note;Color nc=c_muted;
    if(apply_state==1){note=L"Сохранено";nc=c_ok;}else if(apply_state==2){note=L"Ошибка сохранения";nc=c_bad;}
    else if(choice!=saved)note=L"Не сохранено";else if(overlay){note=L"F8 чтобы закрыть";nc=c_dim;}
    text(g,note,R(RX,fy,200,48),Small,nc);
    auto save=R(W-32-160,fy,160,48),reset=R(save.X-10-140,fy,140,48);
    zone(Apply,save);green_button(g,save,Apply,L"СОХРАНИТЬ");
    if(choice)zone(LevelItem,reset);dark_button(g,reset,LevelItem,L"СБРОСИТЬ",Button,choice!=0);
}
void draw_page(G::Graphics& g){
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);g.SetPixelOffsetMode(G::PixelOffsetModeHalf);g.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);g.Clear(c_window);
    if(backdrop){g.SetInterpolationMode(G::InterpolationModeNearestNeighbor);g.DrawImage(backdrop.get(),G::Rect(0,0,px(W),px(H)),0,0,px(W),px(H),G::UnitPixel);}
    g.ScaleTransform(scale,scale);if(page!=0)rect(g,R(0,Head,W,H-Head),Color(200,7,7,8));
    if(page==0)home(g);else if(page==1)settings_page(g);else tiers_page(g);
}
// Startup card: the launcher behind it is blurred (two-step downscale + bicubic upscale reads as a soft gaussian).
std::unique_ptr<G::Bitmap> welcome_blur;ULONGLONG welcome_closing=0;constexpr ULONGLONG WelcomeIn=320,WelcomeOut=220;
float ease_out(ULONGLONG ms,ULONGLONG length){float t=std::clamp(float(ms)/float(length),0.f,1.f);return 1-(1-t)*(1-t)*(1-t);}
void build_welcome_blur(){
    const int w=px(W),h=px(H),mw=std::max(1,w/4),mh=std::max(1,h/4),tw=std::max(1,w/16),th=std::max(1,h/16);
    G::Bitmap sharp(w,h,PixelFormat32bppPARGB),mid(mw,mh,PixelFormat32bppPARGB),tiny(tw,th,PixelFormat32bppPARGB);
    {G::Graphics s(&sharp);draw_page(s);}
    G::ImageAttributes clamp;clamp.SetWrapMode(G::WrapModeTileFlipXY);
    {G::Graphics s(&mid);s.SetInterpolationMode(G::InterpolationModeHighQualityBilinear);s.SetPixelOffsetMode(G::PixelOffsetModeHalf);s.DrawImage(&sharp,G::Rect(0,0,mw,mh),0,0,w,h,G::UnitPixel,&clamp);}
    {G::Graphics s(&tiny);s.SetInterpolationMode(G::InterpolationModeHighQualityBilinear);s.SetPixelOffsetMode(G::PixelOffsetModeHalf);s.DrawImage(&mid,G::Rect(0,0,tw,th),0,0,mw,mh,G::UnitPixel,&clamp);}
    welcome_blur=std::make_unique<G::Bitmap>(w,h,PixelFormat32bppPARGB);G::Graphics s(welcome_blur.get());
    s.SetInterpolationMode(G::InterpolationModeHighQualityBicubic);s.SetPixelOffsetMode(G::PixelOffsetModeHalf);s.DrawImage(&tiny,G::Rect(0,0,w,h),0,0,tw,th,G::UnitPixel,&clamp);
    G::SolidBrush dim(Color(110,4,4,6));s.FillRectangle(&dim,0,0,w,h);
}
void draw_faded(G::Graphics& g,G::Bitmap* image,G::RectF to,float alpha){
    G::ColorMatrix m={{{1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,alpha,0},{0,0,0,0,1}}};G::ImageAttributes fade;fade.SetColorMatrix(&m);
    g.DrawImage(image,to,0,0,float(image->GetWidth()),float(image->GetHeight()),G::UnitPixel,alpha<1?&fade:nullptr);
}
void welcome_card(G::Graphics& g){
    const auto now=GetTickCount64(),waited=smoke?WelcomeDelay:now-welcome_since;const bool ready=waited>=WelcomeDelay&&!welcome_closing;
    const float k=(smoke?1.f:ease_out(now-welcome_since,WelcomeIn))*(welcome_closing?1-ease_out(now-welcome_closing,WelcomeOut):1.f);
    hits.clear();
    {auto state=g.Save();g.ResetTransform();if(welcome_blur)draw_faded(g,welcome_blur.get(),G::RectF(0,0,float(px(W)),float(px(H))),k);g.Restore(state);}
    constexpr float cw=300,ch=132;const auto card=R((W-cw)/2,(H-ch)/2,cw,ch);
    if(ready)zone(WelcomeDismiss,R(0,0,W,H));
    zone(WelcomeLink,card);auto close=R(card.X+cw-36,card.Y+10,26,26);if(ready)zone(WelcomeClose,close);else zone(Nothing,close);
    // The card is drawn into its own bitmap so it can fade and scale as one piece.
    G::Bitmap image(px(int(cw)),px(int(ch)),PixelFormat32bppPARGB);
    {G::Graphics c(&image);c.SetSmoothingMode(G::SmoothingModeAntiAlias);c.SetPixelOffsetMode(G::PixelOffsetModeHalf);c.SetTextRenderingHint(G::TextRenderingHintAntiAliasGridFit);
     c.ScaleTransform(scale,scale);c.Clear(Color(0,0,0,0));auto r=R(0,0,cw,ch);
     fill(c,r,10,Color(244,14,14,17));c.SetClip(r);gradient(c,R(0,0,cw,ch/2),G::PointF(0,0),G::PointF(0,ch/2),Color(14,255,255,255),Color(0,255,255,255));c.ResetClip();
     outline(c,r,10,Color(46,255,255,255));
     logo(c,cw/2-14,20,28);
     const bool hover=hot==WelcomeLink;const Color blue=hover?Color(255,128,190,255):Color(255,70,148,255);
     const float total=pixelfont::width(L"MADE BY @NYAVKE",2),x0=std::floor((cw-total)/2),x1=x0+8*pixelfont::Advance*2;
     ptext(c,L"MADE BY ",x0,64,2,c_text);ptext(c,L"@NYAVKE",x1,64,2,blue);
     if(hover)rect(c,R(x1,81,pixelfont::width(L"@NYAVKE",2),2),blue);
     text(c,L"t.me/nyavke",R(0,92,cw,22),Small,hover?c_body:c_muted,Center);
     // Close control: a ring fills during the delay, then turns into a cross.
     const float cx=cw-23,cy=23,progress=std::min(1.f,float(waited)/WelcomeDelay);
     if(ready){if(hot==WelcomeClose)dot(c,cx,cy,13,Color(40,255,255,255));Color x=hot==WelcomeClose?c_text:c_body;lines(c,{{cx-4.5f,cy-4.5f},{cx+4.5f,cy+4.5f}},x,1.6f);lines(c,{{cx+4.5f,cy-4.5f},{cx-4.5f,cy+4.5f}},x,1.6f);}
     else{G::Pen track(Color(36,255,255,255),2),arc(c_muted,2);arc.SetStartCap(G::LineCapRound);arc.SetEndCap(G::LineCapRound);
          c.DrawEllipse(&track,cx-9,cy-9,18.f,18.f);c.DrawArc(&arc,cx-9,cy-9,18.f,18.f,-90,360*progress);
          text(c,std::to_wstring((WelcomeDelay-waited+999)/1000),R(cx-9,cy-9,18,18),Small,c_muted,Center);}
    }
    const float s=0.94f+0.06f*k,dy=(1-k)*12;const auto to=G::RectF(W/2-cw*s/2,H/2-ch*s/2+dy,cw*s,ch*s);
    for(int i=3;i>=1;--i){auto shadow=to;shadow.Inflate(i*5.f,i*5.f);shadow.Offset(0,8);fill(g,shadow,10+i*5.f,Color(BYTE(24*k),0,0,0));}
    draw_faded(g,&image,to,k);
    window_buttons(g);
}
void render(HDC dc){
    hits.clear();if(welcome&&!welcome_blur)build_welcome_blur();
    hits.clear();G::Graphics g(dc);draw_page(g);
    if(welcome)welcome_card(g);
}

void layout(){
    if(search){SetWindowPos(search,nullptr,px(46),px(Head+18),px(Panel-72),px(20),SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(search,page==2?SW_SHOW:SW_HIDE);if(page!=2&&GetFocus()==search)SetFocus(window);}
    repaint();
}
void load_levels(){levels.clear();auto file=root/L"state/tiers.txt";if(!fs::exists(file))return;std::istringstream in(decode(read(file)));std::string tag;in>>tag;unsigned id,level;
    if(tag=="OVITIER1")while(in>>id>>level)if(level&&heroes.contains(id)&&level<=heroes.at(id).xp.size())levels[id]=level;}
void show_heroes(){try{if(runtime.empty())install();auto path=runtime/L"heroes.tsv";if(!fs::exists(path)){update(L"Сначала запусти игру");page=0;layout();return;}
    heroes=tiers::parse_heroes(decode(read(path)));load_levels();hero_order.clear();for(auto& [id,_]:heroes)hero_order.push_back(id);
    std::sort(hero_order.begin(),hero_order.end(),[](auto a,auto b){return heroes.at(a).name<heroes.at(b).name;});
    if(!heroes.contains(current)&&!hero_order.empty())current=hero_order[0];
    page=2;filter();select_hero(current);layout();if(!smoke&&search)SetFocus(search);
    }catch(const std::exception& e){update(wide(e.what()));page=0;layout();}}
void go(int target){if(target>=2&&heroes.empty())show_heroes();if(target==2)show_heroes();else{page=target;layout();}}
void toggle_overlay(){if(overlay){overlay=false;page=0;SetWindowPos(window,HWND_NOTOPMOST,previous_position.left,previous_position.top,previous_position.right-previous_position.left,previous_position.bottom-previous_position.top,SWP_NOACTIVATE);ShowWindow(window,SW_MINIMIZE);layout();return;}
    DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);if(!attached||pid!=attached)return;if(IsIconic(window)){WINDOWPLACEMENT placement{};placement.length=sizeof(placement);GetWindowPlacement(window,&placement);previous_position=placement.rcNormalPosition;}else GetWindowRect(window,&previous_position);show_heroes();if(page!=2)return;
    overlay=true;ShowWindow(window,SW_RESTORE);SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);SetForegroundWindow(window);if(search)SetFocus(search);}
void click(int id){
    if(id>=LevelItem){choice=unsigned(id-LevelItem);apply_state=0;}
    else if(id>=HeroItem){auto i=size_t(id-HeroItem);if(i<visible.size())select_hero(visible[i]);}
    else switch(id){
    case Minimize:ShowWindow(window,SW_MINIMIZE);break;
    case Close:PostMessageW(window,WM_CLOSE,0,0);break;
    case NavHome:if(overlay)toggle_overlay();else go(0);break;
    case NavSettings:go(1);break;
    case NavTiers:if(!busy){go(2);}break;
    case WelcomeLink:ShellExecuteW(window,L"open",L"https://t.me/nyavke",nullptr,nullptr,SW_SHOWNORMAL);break;
    case WelcomeClose:case WelcomeDismiss:dismiss_welcome();break;
    case Launch:if(busy)cancelled=true;else{if(worker.joinable())worker.join();KillTimer(window,WatchTimer);cancelled=false;failed=false;attached=0;stage=0;busy=true;SetTimer(window,SpinTimer,33,nullptr);worker=std::thread(workflow);}break;
    case Folder:ShellExecuteW(window,L"open",root.c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    case Logs:{auto logs=root/L"state";ShellExecuteW(window,L"open",logs.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}break;
    case Apply:apply();break;
    case TogglePlayer:case ToggleClock:{int i=id==TogglePlayer?0:1;hud_on[i]=!hud_on[i];save_hud();SetTimer(window,HudTimer,16,nullptr);}break;
    case HudReset:++hud_reset;hud_reset_at=GetTickCount64();save_hud();SetTimer(window,HudTimer,16,nullptr);break;
    case SearchBox:SetFocus(search);break;
    }
    repaint();
}
void dismiss_welcome(){if(!smoke&&GetTickCount64()-welcome_since<WelcomeDelay)return;if(welcome_closing)return;
    welcome_closing=GetTickCount64();if(smoke)welcome_closing-=WelcomeOut;SetTimer(window,WelcomeTimer,16,nullptr);repaint();}
void key(WPARAM k,LPARAM flags){
    if(welcome){if(k==VK_RETURN||k==VK_ESCAPE||k==VK_SPACE)dismiss_welcome();return;}
    bool repeat=flags&(1<<30);
    if(page==2&&!visible.empty()){auto at=std::find(visible.begin(),visible.end(),current);int i=at==visible.end()?-1:int(at-visible.begin()),last=int(visible.size())-1;
        int step=k==VK_UP?-1:k==VK_DOWN?1:k==VK_PRIOR?-9:k==VK_NEXT?9:0;if(step){select_hero(visible[std::clamp(i+step,0,last)]);return;}}
    if(page==2&&heroes.contains(current)){auto top=unsigned(heroes.at(current).xp.size());
        if(k==VK_LEFT&&choice>0){--choice;apply_state=0;repaint();return;}if(k==VK_RIGHT&&choice<top){++choice;apply_state=0;repaint();return;}if(k==VK_RETURN&&!repeat){apply();return;}}
    if(page==0&&k==VK_RETURN&&!repeat){click(Launch);return;}
    if(k==VK_ESCAPE&&page==2&&search&&GetWindowTextLengthW(search)){SetWindowTextW(search,L"");return;}
    if(k==VK_ESCAPE&&overlay){toggle_overlay();return;}
    if(k==VK_ESCAPE&&page!=0)go(0);
}
void resume_panel(){try{
    auto file=root/L"state/status.json";if(!fs::exists(file)||fs::file_size(file)>4096)return;auto info=decode(read(file));
    if(info.find("\"stage\":\"virtual_cache_delivered\"")==info.npos)return;auto at=info.find("\"pid\":");if(at==info.npos)return;
    DWORD pid=DWORD(std::strtoul(info.c_str()+at+6,nullptr,10));if(!pid)return;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));if(!process.h||WaitForSingleObject(process,0)!=WAIT_TIMEOUT)return;
    wchar_t path[32768]{};DWORD size=32768;if(!QueryFullProcessImageNameW(process,0,path,&size)||_wcsicmp(fs::path(path).filename().c_str(),L"dota2.exe"))return;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
    if(snapshot.h!=INVALID_HANDLE_VALUE&&Module32FirstW(snapshot,&entry))do{if(!_wcsicmp(entry.szModule,L"InventoryClient.dll")){auto folder=fs::path(entry.szExePath).parent_path();if(folder.parent_path()==root&&folder.filename().wstring().rfind(L"app-",0)==0){found=true;break;}}}while(Module32NextW(snapshot,&entry));
    if(!found)return;attached=pid;SetTimer(window,WatchTimer,2000,nullptr);update(L"Подключено к запущенной Dota",5);
}catch(...){}}
void watch_game(){
    DWORD pid=attached;if(!pid){KillTimer(window,WatchTimer);return;}
    Handle p(OpenProcess(SYNCHRONIZE,FALSE,pid));bool gone=p.h?WaitForSingleObject(p,0)==WAIT_OBJECT_0:GetLastError()==ERROR_INVALID_PARAMETER;if(!gone)return;
    KillTimer(window,WatchTimer);attached=0;stage=-1;
    if(overlay){overlay=false;page=0;SetWindowPos(window,HWND_NOTOPMOST,previous_position.left,previous_position.top,previous_position.right-previous_position.left,previous_position.bottom-previous_position.top,SWP_NOACTIVATE);layout();}
    update(L"Dota 2 закрыта");
}
bool write_icon(const fs::path& path){
    UINT count=0,size=0;G::GetImageEncodersSize(&count,&size);if(!size)return false;std::vector<BYTE> buffer(size);auto codecs=reinterpret_cast<G::ImageCodecInfo*>(buffer.data());G::GetImageEncoders(count,size,codecs);
    const CLSID* png=nullptr;for(UINT i=0;i<count;++i)if(std::wstring(codecs[i].MimeType)==L"image/png")png=&codecs[i].Clsid;if(!png)return false;
    std::vector<std::pair<int,Bytes>> images;
    for(int side:{16,20,24,32,40,48,64,96,128,256}){
        G::Bitmap bitmap(side,side,PixelFormat32bppARGB);{G::Graphics g(&bitmap);g.Clear(Color(0,0,0,0));logo(g,0,0,float(side));}
        if(side<256){
            G::BitmapData px_data;G::Rect all(0,0,side,side);if(bitmap.LockBits(&all,G::ImageLockModeRead,PixelFormat32bppARGB,&px_data)!=G::Ok)return false;
            Bytes data;auto le=[&](unsigned v,int n){for(int i=0;i<n;++i)data.push_back(BYTE(v>>(8*i)));};
            le(40,4);le(unsigned(side),4);le(unsigned(side*2),4);le(1,2);le(32,2);le(0,4);le(0,4);le(0,4);le(0,4);le(0,4);le(0,4);
            for(int y=side-1;y>=0;--y){auto row=static_cast<const BYTE*>(px_data.Scan0)+y*px_data.Stride;data.insert(data.end(),row,row+side*4);}
            bitmap.UnlockBits(&px_data);data.insert(data.end(),size_t((side+31)/32*4*side),BYTE(0));images.emplace_back(side,std::move(data));continue;}
        auto stream=SHCreateMemStream(nullptr,0);if(!stream)return false;bool ok=bitmap.Save(stream,png,nullptr)==G::Ok;STATSTG info{};stream->Stat(&info,STATFLAG_NONAME);
        Bytes data(size_t(info.cbSize.QuadPart));LARGE_INTEGER zero{};stream->Seek(zero,STREAM_SEEK_SET,nullptr);ULONG read=0;stream->Read(data.data(),ULONG(data.size()),&read);stream->Release();
        if(!ok||read!=data.size())return false;images.emplace_back(side,std::move(data));}
    Bytes ico;auto u16=[&](unsigned v){ico.push_back(BYTE(v));ico.push_back(BYTE(v>>8));};auto u32=[&](unsigned v){u16(v&0xffff);u16(v>>16);};
    u16(0);u16(1);u16(unsigned(images.size()));unsigned offset=6+16*unsigned(images.size());
    for(const auto& [side,data]:images){ico.push_back(BYTE(side>=256?0:side));ico.push_back(BYTE(side>=256?0:side));ico.push_back(0);ico.push_back(0);u16(1);u16(32);u32(unsigned(data.size()));u32(offset);offset+=unsigned(data.size());}
    for(const auto& [_,data]:images)ico.insert(ico.end(),data.begin(),data.end());
    write(path,ico);return true;
}
HICON make_icon(int size,G::Bitmap*){
    G::Bitmap bitmap(size,size,PixelFormat32bppARGB);
    {G::Graphics g(&bitmap);g.SetSmoothingMode(G::SmoothingModeAntiAlias);g.SetPixelOffsetMode(G::PixelOffsetModeHalf);g.Clear(Color(0,0,0,0));logo(g,0,0,float(size));}
    HICON icon=nullptr;bitmap.GetHICON(&icon);return icon;
}
int hover_at_cursor(){
    POINT p;GetCursorPos(&p);auto under=WindowFromPoint(p);if(under!=window&&GetParent(under)!=window)return Nothing;ScreenToClient(window,&p);return hit(p.x/scale,p.y/scale);
}
LRESULT CALLBACK edit_proc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_KEYDOWN&&(w==VK_UP||w==VK_DOWN||w==VK_PRIOR||w==VK_NEXT||w==VK_RETURN||w==VK_ESCAPE)){key(w,l);return 0;}
    if(m==WM_CHAR&&(w==VK_RETURN||w==VK_ESCAPE))return 0;
    if(m==WM_SETFOCUS||m==WM_KILLFOCUS)repaint();
    auto result=CallWindowProcW(edit_original,h,m,w,l);
    if(m==WM_PAINT&&!GetWindowTextLengthW(h)){HideCaret(h);auto dc=GetDC(h);RECT r;GetClientRect(h,&r);auto old=SelectObject(dc,editfont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,edit_hint);
        DrawTextW(dc,L"Поиск",-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);ReleaseDC(h,dc);ShowCaret(h);}
    return result;
}
std::unique_ptr<G::Font> make_font(std::initializer_list<const wchar_t*> families,float size){
    for(auto name:families){G::FontFamily family(name);if(family.IsAvailable())return std::make_unique<G::Font>(&family,size,G::FontStyleRegular,G::UnitPixel);}
    return std::make_unique<G::Font>(L"Segoe UI",size,G::FontStyleBold,G::UnitPixel);
}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){
case WM_CREATE:{window=h;
    const auto cond={L"Bahnschrift SemiBold Condensed",L"Bahnschrift SemiBold",L"Segoe UI Semibold"},semi={L"Bahnschrift SemiBold",L"Segoe UI Semibold"};
    fonts[Small]=make_font({L"Segoe UI"},13);fonts[Body]=make_font({L"Segoe UI"},14);fonts[BodyBold]=make_font({L"Segoe UI Semibold"},14);fonts[Brand]=make_font(semi,17);
    fonts[Tab]=make_font(cond,16);fonts[Number]=make_font(semi,15);fonts[Button]=make_font(cond,21);fonts[Title]=make_font(cond,26);fonts[Heading]=make_font(cond,38);
    fonts[Display]=make_font({L"Bahnschrift Bold Condensed",L"Bahnschrift SemiBold Condensed",L"Segoe UI Semibold"},88);
    editfont=CreateFontW(-px(14),0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");editbrush=CreateSolidBrush(edit_bg);
    search=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,0,0,h,reinterpret_cast<HMENU>(INT_PTR(SearchControl)),nullptr,nullptr);
    SendMessageW(search,WM_SETFONT,reinterpret_cast<WPARAM>(editfont),TRUE);SendMessageW(search,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);SendMessageW(search,EM_LIMITTEXT,40,0);
    edit_original=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(search,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(edit_proc)));
    BOOL dark=TRUE;DwmSetWindowAttribute(h,20,&dark,sizeof(dark));int corners=2;DwmSetWindowAttribute(h,33,&corners,sizeof(corners));COLORREF border=RGB(44,49,58);DwmSetWindowAttribute(h,34,&border,sizeof(border));
    {auto art=load_image(art_file(L"library_hero.jpg")),dota_logo=load_image(art_file(L"logo.png"));build_banner(art.get(),dota_logo.get());build_mosaic();build_backdrop();
    icons[0]=make_icon(GetSystemMetrics(SM_CXICON),dota_logo.get());icons[1]=make_icon(GetSystemMetrics(SM_CXSMICON),dota_logo.get());}
    SendMessageW(h,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icons[0]));SendMessageW(h,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(icons[1]));
    load_hud();
    if(!smoke){RegisterHotKey(h,1,MOD_NOREPEAT,VK_F8);curtain_origin=GetTickCount64()+CurtainDelay;SetTimer(h,CurtainTimer,16,nullptr);}return 0;}
case WM_NCHITTEST:{auto result=DefWindowProcW(h,m,w,l);if(result!=HTCLIENT)return result;POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&p);
    float x=p.x/scale,y=p.y/scale;if(y<Bar&&!hit(x,y))return HTCAPTION;return result;}
case WM_NCLBUTTONDBLCLK:if(w==HTCAPTION)return 0;break;
case WM_SETCURSOR:if(LOWORD(l)==HTCLIENT){int at=hover_at_cursor();if(at!=Nothing&&at!=SearchBox&&at!=Scroll&&at!=WelcomeDismiss&&(at<HeroItem||at>=LevelItem)){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}}break;
case WM_ERASEBKGND:return 1;
case WM_PAINT:{PAINTSTRUCT ps;auto dc=BeginPaint(h,&ps);RECT r;GetClientRect(h,&r);auto memory=CreateCompatibleDC(dc);auto bitmap=CreateCompatibleBitmap(dc,r.right,r.bottom);auto old=SelectObject(memory,bitmap);
    render(memory);BitBlt(dc,0,0,r.right,r.bottom,memory,0,0,SRCCOPY);SelectObject(memory,old);DeleteObject(bitmap);DeleteDC(memory);EndPaint(h,&ps);
    if(!dragging){int at=hover_at_cursor();if(at!=hot){hot=at;repaint();}}return 0;}
case WM_SIZE:layout();return 0;
case WM_MOUSEMOVE:{float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;if(dragging){drag_to(y);return 0;}
    auto id=hit(x,y);if(id!=hot){hot=id;repaint();}TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);return 0;}
case WM_CAPTURECHANGED:if(reinterpret_cast<HWND>(l)!=h){dragging=false;pressed=Nothing;repaint();}return 0;
case WM_MOUSELEAVE:if(hot!=Nothing){hot=Nothing;repaint();}return 0;
case WM_LBUTTONDOWN:{float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;pressed=hit(x,y);if(pressed)SetCapture(h);
    if(pressed==Scroll)start_drag(y);else if(pressed>=LevelItem)SetFocus(h);repaint();return 0;}
case WM_LBUTTONUP:{float x=GET_X_LPARAM(l)/scale,y=GET_Y_LPARAM(l)/scale;int id=pressed;bool drag=dragging;pressed=Nothing;dragging=false;ReleaseCapture();
    if(!drag&&id&&id==hit(x,y))click(id);repaint();return 0;}
case WM_MOUSEWHEEL:{POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&p);float x=p.x/scale,y=p.y/scale;
    if(page==2&&x>=LX&&x<LX+LW&&y>=LY&&y<LY+LH){scroll-=GET_WHEEL_DELTA_WPARAM(w)/120.f*Row*3;clamp_scroll();hot=Nothing;repaint();}return 0;}
case WM_KEYDOWN:key(w,l);return 0;
case WM_TIMER:if(w==HudTimer){bool moving=GetTickCount64()-hud_reset_at<1600;
    for(int i=0;i<2;++i){float t=hud_on[i]?1.f:0.f;hud_knob[i]+=(t-hud_knob[i])*0.28f;if(std::abs(t-hud_knob[i])<0.004f)hud_knob[i]=t;else moving=true;}
    repaint();if(!moving)KillTimer(h,HudTimer);}else if(w==WelcomeTimer){auto now=GetTickCount64();
    if(welcome_closing&&now-welcome_closing>=WelcomeOut){welcome=false;welcome_closing=0;welcome_blur.reset();KillTimer(h,WelcomeTimer);hot=hover_at_cursor();}
    else if(!welcome_closing&&now-welcome_since>=WelcomeDelay+WelcomeIn)KillTimer(h,WelcomeTimer);repaint();}else if(w==CurtainTimer)curtain_tick();else if(w==SpinTimer)repaint();else if(w==WatchTimer)watch_game();return 0;
case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(w),edit_fg);SetBkColor(reinterpret_cast<HDC>(w),edit_bg);return reinterpret_cast<LRESULT>(editbrush);
case WM_HOTKEY:toggle_overlay();return 0;
case Update:repaint();return 0;
case Done:if(worker.joinable())worker.join();busy=false;KillTimer(h,SpinTimer);if(closing){DestroyWindow(h);return 0;}if(attached)SetTimer(h,WatchTimer,2000,nullptr);repaint();return 0;
case WM_COMMAND:if(LOWORD(w)==SearchControl&&HIWORD(w)==EN_CHANGE)filter();return 0;
case WM_CLOSE:if(overlay){toggle_overlay();return 0;}if(busy){cancelled=closing=true;update(L"Отменяю...");return 0;}DestroyWindow(h);return 0;
case WM_DESTROY:KillTimer(h,CurtainTimer);KillTimer(h,HudTimer);KillTimer(h,WelcomeTimer);welcome_blur.reset();backdrop.reset();mosaic.reset();UnregisterHotKey(h,1);KillTimer(h,SpinTimer);KillTimer(h,WatchTimer);for(auto& f:fonts)f.reset();banner.reset();for(auto icon:icons)if(icon)DestroyIcon(icon);DeleteObject(editfont);DeleteObject(editbrush);PostQuitMessage(0);return 0;}
return DefWindowProcW(h,m,w,l);}
bool preview(const fs::path& path){int width=px(W),height=px(H);auto screen=GetDC(window),dc=CreateCompatibleDC(screen);BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;
    void* pixels;auto bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);render(dc);
    GdiFlush();BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(bi.bmiHeader);header.bfSize=header.bfOffBits+width*height*4;std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&header),sizeof(header));out.write(reinterpret_cast<char*>(&bi.bmiHeader),sizeof(bi.bmiHeader));out.write(static_cast<char*>(pixels),width*height*4);bool ok=bool(out);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);return ok;}
struct GdiplusSession{ULONG_PTR token=0;GdiplusSession(){G::GdiplusStartupInput input;G::GdiplusStartup(&token,&input,nullptr);}~GdiplusSession(){G::GdiplusShutdown(token);}};
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){try{int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);fs::path image;int preview_page=2;bool install_only=false;
    if(argc>=3&&std::wstring(argv[argc-1]).rfind(L"--curtain=",0)==0){curtain_fixed=ULONGLONG(_wtoi(argv[argc-1]+10));--argc;}
    if(argc>=2){smoke=std::wstring(argv[1])==L"--smoke-test";install_only=std::wstring(argv[1])==L"--install-only";if(smoke&&argc>=3)image=argv[2];if(smoke&&argc>=4){runtime=argv[3];root=runtime.parent_path();}if(install_only&&argc>=3)root=argv[2];if(smoke&&argc>=5)preview_page=_wtoi(argv[4]);}LocalFree(argv);
    if(install_only){install();return 0;}
    {int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);bool icon=count==3&&std::wstring(args[1])==L"--write-icon";fs::path target=icon?fs::path(args[2]):fs::path();LocalFree(args);
     if(icon){GdiplusSession gdiplus;return write_icon(target)?0:1;}}
    Handle mutex;if(!smoke){mutex.h=CreateMutexW(nullptr,FALSE,L"Local\\DotaunlockLauncherV2");if(GetLastError()==ERROR_ALREADY_EXISTS){if(auto existing=FindWindowW(clsname,nullptr)){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}return 0;}install();}welcome=!smoke;
    SetProcessDPIAware();auto dc=GetDC(nullptr);scale=smoke?1.f:GetDeviceCaps(dc,LOGPIXELSX)/96.f;ReleaseDC(nullptr,dc);GdiplusSession gdiplus;
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=proc;cls.lpszClassName=clsname;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));RegisterClassExW(&cls);
    auto hwnd=CreateWindowExW(WS_EX_APPWINDOW,clsname,L"D2UNLOCK",WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,(GetSystemMetrics(SM_CXSCREEN)-px(W))/2,(GetSystemMetrics(SM_CYSCREEN)-px(H))/2,px(W),px(H),nullptr,nullptr,instance,nullptr);
    if(!hwnd)return 2;layout();if(smoke&&!runtime.empty())show_heroes();
    if(smoke&&preview_page==1){page=1;layout();}
    if(smoke&&preview_page==7){page=0;welcome=true;layout();}
    if(smoke){bool ok=!busy&&search&&!image.empty()&&preview(image);if(!runtime.empty())ok=ok&&(page==preview_page||(preview_page==7&&page==0))&&hero_order.size()>100&&heroes.contains(current)&&heroes.at(current).xp.size()+1==31;DestroyWindow(hwnd);return ok?0:3;}
    resume_panel();
    if(welcome){welcome_since=GetTickCount64();SetTimer(hwnd,WelcomeTimer,16,nullptr);}
    ShowWindow(hwnd,show);UpdateWindow(hwnd);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    return 0;
}catch(const std::exception& e){MessageBoxW(nullptr,wide(e.what()).c_str(),L"D2UNLOCK",MB_OK|MB_ICONERROR);return 1;}}

// maya -- Breakout / Arkanoid clone
//
// Half-block pixel rendering, comet trail, multi-hit bricks, particles,
// power-ups, and level progression.
//
// Keys: left/right or h/l=paddle  space=launch/pause  r=restart  q/Esc=quit

#include <maya/internal.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace maya;

static constexpr int BROWS = 8, BPW = 4, BPH = 2, PADDLE_W0 = 6;
static constexpr int TRAIL_LEN = 8, MAX_PARTS = 64, MAX_PWR = 4;
static constexpr float SPEED0 = 0.38f, PAD_SPD = 1.4f;

static constexpr Color ROW_CLR[] = {
    Color::rgb(255,60,60),   Color::rgb(255,140,30), Color::rgb(255,220,40),
    Color::rgb(50,220,80),   Color::rgb(40,210,230), Color::rgb(70,100,255),
    Color::rgb(160,80,220),  Color::rgb(230,70,200),
};
static constexpr int ROW_PTS[] = {80,70,60,50,40,30,20,10};
static Color dimclr(Color c) { return Color::rgb(c.r()/2, c.g()/2, c.b()/2); }

struct Particle { float x,y,vx,vy; int life; Color color; };
enum class Pwr { Wide, Multi, Slow };
struct PowerUp { float x,y; Pwr kind; bool on; };

struct Ball {
    float x,y,vx,vy; bool stuck; int ti;
    float tx[TRAIL_LEN], ty[TRAIL_LEN];
    void clear_trail() { ti=0; for(int i=0;i<TRAIL_LEN;++i){tx[i]=ty[i]=-1;} }
};

// -- State -------------------------------------------------------------------
static std::mt19937 g_rng{42};
static int g_pw, g_ph, g_cols, g_by0;
static std::vector<int> g_bricks;
static Ball g_ball[3]; static int g_nb=1;
static float g_px, g_spd; static int g_padw=PADDLE_W0;
static int g_score=0, g_lives=3, g_level=1;
static bool g_paused=false, g_over=false, g_win=false;
static bool g_kl=false, g_kr=false;
static Particle g_parts[MAX_PARTS];
static PowerUp g_pwr[MAX_PWR];
static int g_wide_t=0, g_slow_t=0;

// -- Styles ------------------------------------------------------------------
static uint16_t s_bg, s_bar, s_bardim, s_accent, s_heart;
static uint16_t s_pad, s_ball, s_brick[BROWS][2], s_trail[TRAIL_LEN], s_pwr[3];

// -- Helpers -----------------------------------------------------------------
static void spawn_particles(float px, float py, Color c) {
    std::uniform_real_distribution<float> v(-0.6f,0.6f);
    int n=0;
    for(auto& p:g_parts) if(p.life<=0&&n<4) { p={px,py,v(g_rng),v(g_rng)-0.3f,12,c}; ++n; }
}
static void spawn_powerup(float px, float py) {
    if(std::uniform_int_distribution<int>(0,4)(g_rng)) return;
    for(auto& pw:g_pwr) if(!pw.on) {
        pw={px,py,Pwr(std::uniform_int_distribution<int>(0,2)(g_rng)),true}; return;
    }
}
static int alive() { int n=0; for(int h:g_bricks) n+=(h>0); return n; }

static void init_level() {
    g_bricks.assign(BROWS*g_cols,0);
    for(int r=0;r<BROWS;++r) { int hp=r<2?2:1; int off=(g_level>1&&r%2)?1:0;
        for(int c=off;c<g_cols;++c) g_bricks[r*g_cols+c]=hp; }
    g_spd=SPEED0+(g_level-1)*0.04f; g_nb=1;
    auto& b=g_ball[0]; b.stuck=true; b.vx=b.vy=0; b.clear_trail();
    g_paused=false; g_wide_t=g_slow_t=0; g_padw=PADDLE_W0;
    for(auto& p:g_parts)p.life=0; for(auto& pw:g_pwr)pw.on=false;
}
static void reset_game() {
    g_score=0;g_lives=3;g_level=1;g_over=g_win=false;g_px=g_pw/2.0f;init_level();
}

// -- Pixel helper: merge half-blocks -----------------------------------------
static const Color BG{Color::rgb(10,10,20)};
static void set_pixel(Canvas& c, int px, int py, uint16_t style) {
    int cx=px, cy=py/2;
    if(cy<0||cy>=c.height()||cx<0||cx>=c.width()) return;
    auto* pool=c.style_pool();
    Color nbg=pool->get(style).bg.value_or(Color::rgb(0,0,0));
    Cell e=c.get(cx,cy); bool top=(py%2==0);
    const Style& cs=pool->get(e.style_id);
    if(e.character==U'\u2580') {
        Color tc=cs.fg.value_or(BG), bc=cs.bg.value_or(BG);
        if(top)tc=nbg; else bc=nbg;
        c.set(cx,cy,U'\u2580',pool->intern(Style{}.with_fg(tc).with_bg(bc)));
    } else {
        Color bg2=cs.bg.value_or(BG);
        c.set(cx,cy,U'\u2580',pool->intern(Style{}.with_fg(top?nbg:bg2).with_bg(top?bg2:nbg)));
    }
}

// -- Resize ------------------------------------------------------------------
static void rebuild(StylePool& pool, int w, int h) {
    g_pw=w; g_ph=h*2;
    g_cols=std::max(1,(g_pw-2)/(BPW+1)); g_by0=4;
    g_px=std::clamp(g_px,(float)g_padw/2,(float)g_pw-(float)g_padw/2);
    s_bg  =pool.intern(Style{}.with_bg(BG));
    s_bar =pool.intern(Style{}.with_bg(Color::rgb(20,20,30)).with_fg(Color::rgb(120,120,140)));
    s_bardim=pool.intern(Style{}.with_bg(Color::rgb(20,20,30)).with_fg(Color::rgb(70,70,90)));
    s_accent=pool.intern(Style{}.with_bg(Color::rgb(20,20,30)).with_fg(Color::rgb(255,200,60)).with_bold());
    s_heart =pool.intern(Style{}.with_bg(Color::rgb(20,20,30)).with_fg(Color::rgb(255,60,80)));
    s_pad =pool.intern(Style{}.with_bg(Color::rgb(240,240,255)));
    s_ball=pool.intern(Style{}.with_bg(Color::rgb(255,255,200)));
    for(int r=0;r<BROWS;++r) {
        s_brick[r][0]=pool.intern(Style{}.with_bg(ROW_CLR[r]));
        s_brick[r][1]=pool.intern(Style{}.with_bg(dimclr(ROW_CLR[r])));
    }
    for(int i=0;i<TRAIL_LEN;++i) { float t=1.0f-(float)i/TRAIL_LEN;
        auto v=(uint8_t)(180*t*t);
        s_trail[i]=pool.intern(Style{}.with_bg(Color::rgb(v,v,(uint8_t)(v/2)))); }
    s_pwr[0]=pool.intern(Style{}.with_bg(Color::rgb(30,180,60)).with_fg(Color::rgb(255,255,255)).with_bold());
    s_pwr[1]=pool.intern(Style{}.with_bg(Color::rgb(50,100,255)).with_fg(Color::rgb(255,255,255)).with_bold());
    s_pwr[2]=pool.intern(Style{}.with_bg(Color::rgb(220,200,40)).with_fg(Color::rgb(0,0,0)).with_bold());
    if(g_bricks.empty()) reset_game();
}

// -- Event -------------------------------------------------------------------
static bool handle(const Event& ev) {
    if(key(ev,'q')||key(ev,SpecialKey::Escape)) return false;
    on(ev,'r',[]{reset_game();});
    on(ev,' ',[]{
        if(g_over||g_win){reset_game();return;}
        if(g_ball[0].stuck){ auto& b=g_ball[0]; b.stuck=false;
            b.vx=g_spd*std::uniform_real_distribution<float>(-0.4f,0.4f)(g_rng);
            b.vy=-g_spd;
        } else g_paused=!g_paused;
    });
    if(key(ev,SpecialKey::Left)||key(ev,'h')) g_kl=true;
    if(key(ev,SpecialKey::Right)||key(ev,'l')) g_kr=true;
    return true;
}

// -- Tick --------------------------------------------------------------------
static void tick() {
    if(g_paused||g_over||g_win) return;
    float pdx=0; if(g_kl)pdx-=PAD_SPD; if(g_kr)pdx+=PAD_SPD;
    g_kl=g_kr=false;
    float hw=g_padw/2.0f;
    g_px=std::clamp(g_px+pdx,hw,(float)g_pw-hw);
    if(g_wide_t>0&&--g_wide_t==0) g_padw=PADDLE_W0;
    if(g_slow_t>0) --g_slow_t;
    float ptop=g_ph-4.0f;
    int bx0=(g_pw-(g_cols*(BPW+1)-1))/2;

    for(int bi=0;bi<g_nb;++bi) {
        auto& b=g_ball[bi];
        if(b.stuck){b.x=g_px;b.y=ptop-1;continue;}
        b.tx[b.ti]=b.x; b.ty[b.ti]=b.y; b.ti=(b.ti+1)%TRAIL_LEN;
        float sm=g_slow_t>0?0.6f:1.0f;
        b.x+=b.vx*sm; b.y+=b.vy*sm;
        // walls
        if(b.x<0){b.x=0;b.vx=std::abs(b.vx);}
        if(b.x>=g_pw-1){b.x=(float)g_pw-1;b.vx=-std::abs(b.vx);}
        if(b.y<0){b.y=0;b.vy=std::abs(b.vy);}
        // paddle
        if(b.vy>0&&b.y>=ptop&&b.y<ptop+2){
            float rel=std::clamp((b.x-g_px)/hw,-1.0f,1.0f);
            float sp=std::sqrt(b.vx*b.vx+b.vy*b.vy)+0.005f;
            float a=rel*1.1f; b.vx=sp*std::sin(a); b.vy=-sp*std::cos(a); b.y=ptop-1;
        }
        // bricks
        int col=(int)(b.x-bx0)/(BPW+1), row=(int)(b.y-g_by0)/(BPH+1);
        if(row>=0&&row<BROWS&&col>=0&&col<g_cols){
            int idx=row*g_cols+col;
            if(g_bricks[idx]>0){
                g_bricks[idx]--;
                if(!g_bricks[idx]){
                    g_score+=ROW_PTS[row];
                    float cx=bx0+col*(BPW+1)+BPW/2.0f, cy=g_by0+row*(BPH+1)+BPH/2.0f;
                    spawn_particles(cx,cy,ROW_CLR[row]); spawn_powerup(cx,cy);
                }
                float cx=bx0+col*(BPW+1)+BPW/2.0f, cy=g_by0+row*(BPH+1)+BPH/2.0f;
                if(std::abs(b.x-cx)*BPH>std::abs(b.y-cy)*BPW) b.vx=-b.vx; else b.vy=-b.vy;
            }
        }
        // lost
        if(b.y>=g_ph){
            if(bi==0){g_lives--;if(g_lives<=0){g_over=true;return;}
                b.stuck=true;b.clear_trail();g_nb=1;
            } else { g_ball[bi]=g_ball[--g_nb]; --bi; }
        }
    }
    // power-ups
    float pl=g_px-hw, pr=g_px+hw;
    for(auto& pw:g_pwr){
        if(!pw.on)continue; pw.y+=0.3f;
        if(pw.y>=ptop&&pw.y<ptop+3&&pw.x>=pl&&pw.x<=pr){
            pw.on=false;
            if(pw.kind==Pwr::Wide){g_padw=PADDLE_W0+4;g_wide_t=600;}
            else if(pw.kind==Pwr::Multi&&g_nb<3){
                auto& nb=g_ball[g_nb++]; nb=g_ball[0]; nb.stuck=false;
                nb.vx=-nb.vx; nb.clear_trail();
            } else if(pw.kind==Pwr::Slow) g_slow_t=300;
        }
        if(pw.y>=g_ph)pw.on=false;
    }
    for(auto& p:g_parts){if(p.life<=0)continue;p.x+=p.vx;p.y+=p.vy;p.vy+=0.04f;p.life--;}
    if(!alive()){g_level++;if(g_level>5){g_win=true;return;}init_level();}
}

// -- Paint -------------------------------------------------------------------
static void paint(Canvas& canvas, int w, int h) {
    if(w!=g_pw||h*2!=g_ph) return;
    for(int y=0;y<h-1;++y) for(int x=0;x<w;++x) canvas.set(x,y,U' ',s_bg);
    tick();
    int bx0=(g_pw-(g_cols*(BPW+1)-1))/2;
    // bricks
    for(int r=0;r<BROWS;++r) for(int c=0;c<g_cols;++c){
        int hp=g_bricks[r*g_cols+c]; if(hp<=0) continue;
        uint16_t st=s_brick[r][hp>=2?0:(r<2?1:0)];
        int px=bx0+c*(BPW+1), py=g_by0+r*(BPH+1);
        for(int dy=0;dy<BPH;++dy) for(int dx=0;dx<BPW;++dx) set_pixel(canvas,px+dx,py+dy,st);
    }
    // particles
    for(auto& p:g_parts){ if(p.life<=0)continue; float br=(float)p.life/12.0f;
        uint16_t ps=canvas.style_pool()->intern(Style{}.with_bg(
            Color::rgb((uint8_t)(p.color.r()*br),(uint8_t)(p.color.g()*br),(uint8_t)(p.color.b()*br))));
        set_pixel(canvas,(int)p.x,(int)p.y,ps);
    }
    // power-ups
    static const char32_t pl[]={U'W',U'M',U'S'};
    for(auto& pw:g_pwr){ if(!pw.on)continue;
        int cx=(int)pw.x,cy=(int)pw.y/2;
        if(cy>=0&&cy<h-1&&cx>=0&&cx<w) canvas.set(cx,cy,pl[(int)pw.kind],s_pwr[(int)pw.kind]);
    }
    // trail + balls
    for(int bi=0;bi<g_nb;++bi){ auto& b=g_ball[bi];
        for(int i=0;i<TRAIL_LEN;++i){ int ti=(b.ti-1-i+TRAIL_LEN)%TRAIL_LEN;
            if(b.tx[ti]>=0) set_pixel(canvas,(int)b.tx[ti],(int)b.ty[ti],s_trail[i]); }
        set_pixel(canvas,(int)b.x,(int)b.y,s_ball);
    }
    // paddle
    float hw2=g_padw/2.0f; float ptop=g_ph-4.0f;
    for(int px=(int)(g_px-hw2);px<(int)(g_px+hw2);++px)
        for(int dy=0;dy<2;++dy) set_pixel(canvas,px,(int)ptop+dy,s_pad);

    // overlays
    auto ctr=[&](const char*m,int row,uint16_t s){
        canvas.write_text((w-(int)std::strlen(m))/2,row,m,s);};
    if(g_over||g_win){
        uint16_t ts=canvas.style_pool()->intern(Style{}.with_bg(BG).with_fg(
            g_over?Color::rgb(255,60,60):Color::rgb(100,255,120)).with_bold());
        uint16_t ds=canvas.style_pool()->intern(Style{}.with_bg(BG).with_fg(Color::rgb(160,160,180)));
        ctr(g_over?"GAME OVER":"YOU WIN!",h/2-1,ts);
        char buf[48]; std::snprintf(buf,sizeof(buf),g_over?"Score: %d":"Final Score: %d",g_score);
        ctr(buf,h/2,ds); ctr("[space] restart  [q] quit",h/2+2,ds);
    }
    // status bar
    int by=h-1;
    for(int x=0;x<w;++x) canvas.set(x,by,U' ',s_bar);
    char bar[256]; std::snprintf(bar,sizeof(bar),
        "BREAKOUT \xe2\x94\x82 Score: %d \xe2\x94\x82 Lives: ",g_score);
    canvas.write_text(1,by,bar,s_bardim);
    canvas.write_text(1,by,"BREAKOUT",s_accent);
    const char*lp=std::strstr(bar,"Lives: ");
    int hx=lp?1+(int)(lp-bar)+7:20;
    for(int i=0;i<g_lives;++i) canvas.write_text(hx+i*2,by,"\xe2\x99\xa5",s_heart);
    char rest[128]; std::snprintf(rest,sizeof(rest),
        "\xe2\x94\x82 Level: %d \xe2\x94\x82 [\xe2\x86\x90\xe2\x86\x92] move \xe2\x94\x82 [q] quit",g_level);
    canvas.write_text(hx+g_lives*2+1,by,rest,s_bardim);
}

int main() {
    (void)canvas_run(CanvasConfig{.fps=60,.title="breakout"},rebuild,handle,paint);
}

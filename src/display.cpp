#include "tracker.h"
#include "coast.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <algorithm>

bool demoMode;
namespace {
constexpr int Side = 680, C = Side / 2, R = 310, Top = 20, PanelX = 698;
constexpr uint32_t Bg = 0x09131c, Sea = 0x102737, Land = 0x303933, Shore = 0x607060;
constexpr uint32_t Ink = 0xe2f2ef, Dim = 0x96b0b6, Cyan = 0x65ead6, Gold = 0xffce73;
constexpr uint32_t Grid = 0x35595f, Card = 0x122630, Divider = 0x28414b;
M5Canvas base(&M5.Display), scope(&M5.Display), panel(&M5.Display);
bool ready, pinned, panelDirty = true;
Aircraft livePlanes[MaxAircraft], demoPlanes[8];
Aircraft *planes=livePlanes;
int liveCount;
int count, selected = -1, cachedRange, nearby[3]{-1,-1,-1};
char statusText[64] = "Waiting for ADS-B";
time_t observed;
uint32_t lastFrame, lastPanel, lastFocus;
float sweepAngle;
uint8_t landscapeRotation, tiltSamples;
const char *demoCalls[] = {"DEMO201","DEMO452","DEMO018","DEMO737","DEMO316","DEMO082","DEMO109","DEMO910"};
struct Place { const char *label; float lat, lon; bool airport; };
constexpr Place places[] = {
    {"TOKYO",35.681,139.767,false},{"Yokohama",35.444,139.638,false},
    {"Chiba",35.608,140.106,false},{"Kisarazu",35.381,139.924,false},
    {"Yokosuka",35.282,139.672,false},{"Tateyama",34.996,139.870,false},
    {"Saitama",35.861,139.646,false},{"Tsukuba",36.083,140.112,false},
    {"Odawara",35.256,139.155,false},{"Oshima",34.783,139.383,false},
    {"NRT",35.772,140.393,true},
    {"OSAKA",34.694,135.502,false},{"Kobe",34.690,135.196,false},
    {"Wakayama",34.230,135.171,false},{"Nara",34.685,135.805,false},
    {"Awaji",34.435,134.915,false},{"Kyoto",35.011,135.768,false},
    {"ITM",34.785,135.438,true},{"UKB",34.633,135.224,true},
    {"Ginowan",26.281,127.778,false},{"Nago",26.591,127.978,false},
    {"Motobu",26.658,127.902,false},{"Itoman",26.124,127.665,false},
    {"Tokashiki",26.197,127.364,false},{"UEO",26.363,126.714,true}
};

void textAt(M5Canvas &canvas, int x, int y, const String &s, uint32_t color, int size=1, bool large=false) {
    canvas.setFont(large ? &fonts::Font4 : &fonts::Font2);
    canvas.setTextSize(size);
    canvas.setTextColor(color);
    canvas.drawString(s, x, y);
}

int px(Point p) { return C + lroundf(p.east * R / settings.range); }
int py(Point p) { return C - lroundf(p.north * R / settings.range); }
float miles(const Aircraft &a) { return hypotf(a.pos.east, a.pos.north); }
bool visible(int i) { return i >= 0 && i < count && miles(planes[i]) <= settings.range; }
bool stale() { return !demoMode && observed && difftime(time(nullptr),observed) > PollSeconds+45; }

void drawBase() {
    cachedRange = settings.range;
    const auto &region = regions[settings.region];
    base.fillScreen(Bg);
    base.fillCircle(C,C,R,Sea);
    for (int dy = -R; dy <= R; ++dy) {
        int row = std::clamp(int((.5f + dy * settings.range / (120.f * R)) * CoastSize),0,CoastSize-1);
        int dx = int(sqrtf(R*R - dy*dy));
        for (unsigned j=region.rows[row]; j<region.rows[row+1]; j+=2) {
            const float factor = 120.f * R / (CoastSize * settings.range);
            int a = C + lroundf((region.spans[j] - CoastSize/2) * factor);
            int b = C + lroundf((region.spans[j+1] - CoastSize/2) * factor);
            int left = std::max(C-dx,a), right = std::min(C+dx,b);
            if (left >= right) continue;
            base.drawFastHLine(left,C+dy,right-left,Land);
            if (a == left) base.drawPixel(a,C+dy,Shore);
            if (b == right) base.drawPixel(b,C+dy,Shore);
        }
    }
    for (int i=1;i<=4;i++) base.drawCircle(C,C,R*i/4,i==4 ? Cyan : Grid);
    base.drawFastVLine(C,C-R,R*2,Grid);
    base.drawFastHLine(C-R,C,R*2,Grid);
    for (int deg=0;deg<360;deg+=5) {
        float a=deg*DEG_TO_RAD;
        int len=deg%30==0 ? 9 : 4;
        base.drawLine(C+sin(a)*(R-len),C-cos(a)*(R-len),C+sin(a)*R,C-cos(a)*R,
                      deg%30==0 ? Cyan : Grid);
    }
    for (auto &place : places) {
        Point p = projectPosition(region,place.lat,place.lon);
        int x=px(p), y=py(p);
        if (hypotf(x-C,y-C)>R-14) continue;
        base.fillCircle(x,y,place.airport ? 3 : 2,place.airport ? Gold : Dim);
        int width = strlen(place.label)*8;
        if (hypotf(x+width-C,y-C) < R-8) textAt(base,x+5,y+2,place.label,place.airport ? Gold : Dim);
    }
    base.fillCircle(C,C,3,Gold);
    textAt(base,C+5,C+2,region.airport,Gold);
    textAt(base,C-6,4,"N",Ink);
    textAt(base,C-5,Side-22,"S",Dim);
    textAt(base,8,C-8,"W",Dim);
    textAt(base,Side-20,C-8,"E",Dim);
    base.drawRoundRect(8,Side-37,94,34,8,Grid);
    textAt(base,16,Side-29,String(settings.range)+" NM  >",Cyan);
}

void demoPositions() {
    count = sizeof(demoCalls)/sizeof(demoCalls[0]);
    const float sec = millis()/1000.f;
    for (int i=0;i<count;i++) {
        Aircraft &a = planes[i];
        if (!a.call[0]) {
            strlcpy(a.call,demoCalls[i],sizeof(a.call));
            snprintf(a.hex,sizeof(a.hex),"D%05X",i+1);
            strlcpy(a.type,i%2 ? "B738" : "A320",sizeof(a.type));
            a.altitude = 3200 + i*2700; a.speed=220+i*16;
        }
        float h=(i*97+25)*DEG_TO_RAD;
        float t=fmodf(sec*a.speed/3600.f + i*9 + 19,72.f)-36;
        float d=(i%4-1.5f)*6.5f;
        a.heading=i*97+25; a.heading=fmodf(a.heading,360);
        auto position = [&](float along) -> Point {
            return {along*sinf(h)+d*cosf(h),along*cosf(h)-d*sinf(h)};
        };
        a.pos=position(t);
        a.trailCount=0;
        for (int k=4;k>=1;k--) if (t-k*.6f >= -36) a.trail[a.trailCount++]=position(t-k*.6f);
    }
}

void chooseNext() {
    for (int n=1;n<=count;n++) {
        int i=(selected+n)%count;
        if (visible(i)) { selected=i; lastFocus=millis(); panelDirty=true; return; }
    }
    selected=-1;
}

void selectPlane(int index) {
    selected=index; pinned=true; lastFocus=millis(); panelDirty=true;
}

void drawAircraft(int i) {
    const auto &a=planes[i];
    int x=px(a.pos), y=py(a.pos);
    bool focus=i==selected;
    uint32_t color=focus ? Gold : (stale() ? Dim : Cyan);
    int lastX=x, lastY=y;
    for (int n=a.trailCount-1;n>=0;n--) {
        int tx=px(a.trail[n]),ty=py(a.trail[n]);
        if (hypotf(tx-C,ty-C)>R) break;
        scope.drawLine(lastX,lastY,tx,ty,focus ? 0xa18b5a : Grid);
        scope.fillCircle(tx,ty,2,focus ? Gold : Dim);
        lastX=tx; lastY=ty;
    }
    if (isfinite(a.heading)) {
        const float h=a.heading*DEG_TO_RAD,s=sinf(h),c=cosf(h), scale=focus ? 1.22f : 1.f;
        auto vertex=[&](int right,int forward) {
            return std::pair<int,int>{x+lroundf((right*c+forward*s)*scale),y+lroundf((right*s-forward*c)*scale)};
        };
        auto tri=[&](int r1,int f1,int r2,int f2,int r3,int f3) {
            auto p=vertex(r1,f1), q=vertex(r2,f2), r=vertex(r3,f3);
            scope.fillTriangle(p.first,p.second,q.first,q.second,r.first,r.second,color);
        };
        tri(0,13,-2,-10,2,-10); tri(0,5,-12,-4,12,-4); tri(0,-5,-5,-10,5,-10);
    } else {
        scope.drawCircle(x,y,5,color); scope.fillCircle(x,y,2,color);
    }
    if (focus) {
        scope.drawCircle(x,y,19,Gold);
        int lx=std::clamp(x+23,12,Side-158), ly=std::clamp(y-33,22,Side-43);
        int w=strlen(a.call)*10+18;
        scope.drawLine(x,y-16,lx+6,ly+22,Gold);
        scope.fillRoundRect(lx,ly,w,25,4,Bg);
        scope.drawRoundRect(lx,ly,w,25,4,Gold);
        textAt(scope,lx+6,ly+4,a.call,Ink);
    }
}

void drawScope() {
    base.pushSprite(&scope,0,0);
    // 等間隔の点を薄く残し、地名の上を透かして走査する。
    for (int n=10;n>=0;n--) {
        float a=(sweepAngle-n*1.15f)*DEG_TO_RAD;
        float sx=sinf(a), sy=-cosf(a);
        if (!n) scope.drawLine(C,C,C+sx*(R-2),C+sy*(R-2),0x58c9ba);
        else for (int r=8+n%3;r<R-2;r+=4+n/2)
            scope.drawPixel(C+sx*r,C+sy*r,n<4 ? 0x439485 : 0x30574f);
    }
    for (int i=0;i<count;i++) if (i!=selected && visible(i)) drawAircraft(i);
    if (visible(selected)) drawAircraft(selected);
    scope.pushSprite(0,Top);
}

String value(float n) { return isfinite(n) ? String(lroundf(n)) : String("--"); }

void drawPanel() {
    auto &p=panel;
    p.fillScreen(Bg);
    p.drawRoundRect(0,6,285,44,8,Grid);
    textAt(p,8,16,regions[settings.region].title,Cyan,1,true);
    textAt(p,260,18,">",Cyan);
    time_t now=time(nullptr);
    char clockText[12]="--:--";
    if (now>1700000000) { struct tm tm; localtime_r(&now,&tm); strftime(clockText,sizeof(clockText),"%H:%M",&tm); }
    textAt(p,397,17,clockText,Ink,1,true);
    textAt(p,483,26,"JST",Dim);
    textAt(p,0,59,"Flight radar",Ink,2,true);
    p.fillRoundRect(396,68,144,38,16,Card);
    p.drawRoundRect(396,68,144,38,16,demoMode ? Gold : Cyan);
    textAt(p,414,76,demoMode ? "DEMO  /  LIVE" : "LIVE  /  DEMO",demoMode ? Gold : Cyan);
    int visibleCount=0;
    for(int i=0;i<count;i++) if(visible(i)) visibleCount++;
    String state=demoMode ? "Simulated traffic" : String(statusText);
    if (!demoMode && observed) state += "  /  " + String(std::max(0L,long(now-observed)))+"s ago";
    textAt(p,0,123,state,stale() ? Gold : Dim);
    p.fillRoundRect(0,162,548,235,14,Card);
    textAt(p,22,178,"IN FOCUS",Cyan);
    textAt(p,387,176,pinned ? "PINNED  [x]" : (settings.cycle ? "AUTO FOCUS" : "MANUAL"),pinned ? Gold : Dim);
    const Aircraft *a=visible(selected) ? &planes[selected] : nullptr;
    textAt(p,20,210,a ? a->call : "No traffic",a ? Gold : Ink,2,true);
    if (a) textAt(p,23,270,String(a->type[0] ? a->type : "Type unknown")+"   /   "+a->hex,Dim);
    p.drawFastHLine(22,303,500,Divider);
    const char *labels[]={"ALT  ft","GS  kt","TRK  deg","DIST  NM"};
    String values[] = {a && a->ground ? "GND" : value(a ? a->altitude : NAN),
                       value(a ? a->speed : NAN),value(a ? a->heading : NAN),a ? String(miles(*a),1) : "--"};
    for(int n=0;n<4;n++) { textAt(p,22+n*132,314,labels[n],Dim); textAt(p,22+n*132,341,values[n],Ink,1,true); }
    textAt(p,0,418,"Nearby aircraft",Ink,1,true);
    textAt(p,427,423,String(visibleCount)+" in range",Cyan);
    int used=0;
    std::fill(std::begin(nearby),std::end(nearby),-1);
    for(int i=0;i<count && used<3;i++) {
        if(!visible(i) || i==selected) continue;
        nearby[used]=i;
        int y=467+used++*49;
        p.drawFastHLine(0,y-8,546,Divider);
        textAt(p,8,y,planes[i].call,Ink,1,true);
        textAt(p,255,y+4,planes[i].ground ? "GND" : value(planes[i].altitude)+" ft",Dim);
        textAt(p,438,y+4,String(miles(planes[i]),1)+" NM",Cyan);
    }
    if (!used) textAt(p,8,474,"No other aircraft in range",Dim);
    p.drawFastHLine(0,620,546,Divider);
    textAt(p,0,633,WiFi.isConnected() ? String("Settings   http://")+WiFi.localIP().toString() :
           "USB setup   /   Open web/settings.html",Ink);
    textAt(p,0,664,demoMode ? "DEMO DATA    /    Natural Earth" : "adsb.lol  (ODbL)    /    Natural Earth",Dim);
    p.pushSprite(PanelX,0);
}

void updateOrientation() {
    if (!M5.Imu.isEnabled() || !(M5.Imu.update() & m5::IMU_Class::sensor_mask_accel) ||
        M5.Touch.getCount()) {
        tiltSamples = 0;
        return;
    }
    const auto a = M5.Imu.getImuData().accel;
    const float g = sqrtf(a.x*a.x + a.y*a.y + a.z*a.z);
    const bool flipped = M5.Display.getRotation() != landscapeRotation;
    // 横画面の上下はIMUのX軸。±sin(35°)を越えるまで現在の向きを保持する。
    const float opposite = flipped ? a.x : -a.x;
    if (!(g > .8f && g < 1.2f && opposite >= .574f*g && opposite > fabsf(a.y))) {
        tiltSamples = 0;
        return;
    }
    if (++tiltSamples < 3) return;
    tiltSamples = 0;
    M5.Display.setRotation(M5.Display.getRotation() ^ 2);
    M5.Display.fillScreen(Bg);
    panelDirty = true;
}
}

void initializeDisplay() {
    M5.Display.setRotation(3);
    if (M5.Display.width()<M5.Display.height()) M5.Display.setRotation(2);
    landscapeRotation = M5.Display.getRotation();
    M5.Display.fillScreen(Bg);
    for (auto *s : {&base,&scope,&panel}) { s->setPsram(true); s->setColorDepth(16); }
    ready=base.createSprite(Side,Side) && scope.createSprite(Side,Side) && panel.createSprite(562,710);
    if (!ready) M5.Display.println("Radar frame buffer unavailable");
    Serial.printf("{\"event\":\"display\",\"ready\":%s,\"width\":%d,\"height\":%d}\n",
                  ready ? "true" : "false",M5.Display.width(),M5.Display.height());
}

void setFeedStatus(const char *message) {
    if (strcmp(statusText,message)) { strlcpy(statusText,message,sizeof(statusText)); panelDirty=true; }
}

void setDemo(bool enabled) {
    if (demoMode==enabled) return;
    demoMode=enabled;
    planes=enabled ? demoPlanes : livePlanes;
    count=enabled ? 0 : liveCount;
    selected=-1; pinned=false;
    panelDirty=true;
}

void clearRegionTraffic() {
    liveCount = count = 0;
    observed = 0;
    selected = -1;
    pinned = false;
    std::fill(std::begin(nearby),std::end(nearby),-1);
    cachedRange = 0;
    panelDirty = true;
    setFeedStatus("Waiting for ADS-B");
}

void acceptFeed(const Feed &data) {
    if (observed && data.observed < observed) { setFeedStatus("Older feed ignored"); return; }
    static Aircraft next[MaxAircraft];
    char previous[10]{};
    if (!demoMode && selected>=0) strlcpy(previous,planes[selected].hex,sizeof(previous));
    int found=-1;
    for(int i=0;i<data.count;i++) {
        auto &a=next[i]; a=data.aircraft[i];
        if (!strcmp(previous,a.hex)) found=i;
        for(int j=0;j<liveCount;j++) if (!strcmp(a.hex,livePlanes[j].hex)) {
            const auto &old=livePlanes[j];
            if (difftime(data.observed,observed)<=2*PollSeconds+30) {
                bool moved=hypotf(old.pos.east-a.pos.east,old.pos.north-a.pos.north)>.03f;
                int start=moved && old.trailCount==5 ? 1 : 0;
                for(int k=start;k<old.trailCount;k++) a.trail[a.trailCount++]=old.trail[k];
                if(moved) a.trail[a.trailCount++]=old.pos;
            }
            break;
        }
    }
    liveCount=data.count;
    std::copy_n(next,liveCount,livePlanes);
    if (!demoMode) {
        count=liveCount; selected=found;
        if (!visible(selected)) { pinned=false; selected=-1; }
    }
    observed=data.observed;
    setFeedStatus("ADS-B received");
    panelDirty=true;
}

void updateTouch() {
    auto t=M5.Touch.getDetail();
    if(!ready || !t.wasPressed()) return;
    if(t.x>=PanelX) {
        int x=t.x-PanelX;
        if(x<=290 && t.y<=55) {
            settings.region = (settings.region + 1) % RegionCount;
            onRegionChanged();
        } else if(x>=390 && t.y>=60 && t.y<=116) setDemo(!demoMode);
        else if(t.y>=162 && t.y<=397 && selected>=0) {
            pinned=!pinned; lastFocus=millis(); panelDirty=true;
        } else if(t.y>=459 && t.y<606) {
            int i=nearby[(t.y-459)/49];
            if(visible(i)) selectPlane(i);
        }
    } else if (t.x <= 110 && t.y >= Top+Side-45 && t.y < Top+Side+4) {
        settings.range = settings.range == 60 ? 20 : settings.range + 20;
    } else {
        int closest=-1; float best=27;
        for(int i=0;i<count;i++) if(visible(i)) {
            float d=hypotf(t.x-px(planes[i].pos),t.y-Top-py(planes[i].pos));
            if(d<best) { best=d; closest=i; }
        }
        if(closest>=0) selectPlane(closest);
    }
}

void renderTracker() {
    uint32_t now=millis(), delta=now-lastFrame;
    if(!ready || delta<90) return;
    lastFrame=now;
    updateOrientation();
    if(cachedRange!=settings.range) { drawBase(); panelDirty=true; }
    if(demoMode) demoPositions();
    if(!visible(selected)) { selected=-1; pinned=false; chooseNext(); }
    else if(!pinned && settings.cycle && now-lastFocus>=unsigned(settings.cycle)*1000) chooseNext();
    sweepAngle=fmodf(sweepAngle+std::min(delta,500UL)*.36f/settings.sweep,360.f);
    drawScope();
    if(panelDirty || now-lastPanel>=1000) { drawPanel(); lastPanel=now; panelDirty=false; }
}

void describeRadar(JsonObject out) {
    out["displayReady"]=ready;
    out["imuReady"]=M5.Imu.isEnabled();
    out["rotation"]=M5.Display.getRotation();
    out["mode"]=demoMode ? "demo" : "live";
    out["status"]=demoMode ? "Simulated traffic" : statusText;
    out["aircraft"]=count;
    out["observed"]=demoMode ? 0 : observed;
    out["frameMs"]=lastFrame;
}

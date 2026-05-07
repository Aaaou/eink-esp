#include "portal_service.h"

#include "cJSON.h"
#include "eink_panel.h"
#include "eink_scene.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "project_defaults.h"
#include "recording_service.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define PORTAL_NAMESPACE "ink_portal"
#define PORTAL_KEY_SSID "ssid"
#define PORTAL_KEY_PASSWORD "password"
#define PORTAL_AP_MAX_CONNECTIONS 4
#define PORTAL_MAX_SCAN_RESULTS 12
#define PORTAL_FRAME_MODE_TRI 0
#define PORTAL_FRAME_MODE_BW_FAST 1
#define PORTAL_FRAME_UPLOAD_LEN (1U + (EINK_PANEL_BUF_LEN * 2U))

static const char *TAG = "portal_service";

typedef enum {
    PORTAL_STATE_IDLE = 0,
    PORTAL_STATE_PROVISIONING,
    PORTAL_STATE_CONNECTING,
    PORTAL_STATE_CONNECTED,
    PORTAL_STATE_ERROR,
} portal_state_t;

typedef struct {
    httpd_handle_t server;
    esp_netif_t *ap_netif;
    esp_netif_t *sta_netif;
    TaskHandle_t autoconnect_task;
    portal_state_t state;
    bool wifi_initialized;
    bool portal_enabled;
    bool sta_connected;
    bool autoconnect_requested;
    bool credentials_loaded;
    bool sntp_started;
    bool calendar_task_started;
    TaskHandle_t calendar_task;
    char ssid[33];
    char password[65];
    char ip_address[16];
} portal_runtime_t;

static portal_runtime_t s_portal;
static uint8_t s_upload_buffer[PORTAL_FRAME_UPLOAD_LEN];

static const char s_index_html[] =
"<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Ink ESP</title><meta http-equiv='Cache-Control' content='no-store, no-cache, must-revalidate, max-age=0'><style>"
":root{--bg:#08121f;--card:rgba(10,20,38,.92);--line:rgba(255,255,255,.14);--ink:#f4f7fb;--muted:#b6c7da;--accent:#59d1ff;--accent2:#8af4b0;--danger:#ff7d79}"
"*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:14px;font-family:Arial,'Microsoft YaHei',sans-serif;color:var(--ink);background:linear-gradient(145deg,#08121f,#14324d)}.shell{max-width:1040px;margin:0 auto;display:grid;grid-template-columns:1fr 1fr;gap:14px}.panel{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px}.row,.toolbar{display:flex;gap:8px;align-items:center;justify-content:space-between;flex-wrap:wrap}h1{margin:10px 0;font-size:28px}h2{margin:0 0 10px;font-size:20px}.hint,.msg,.meta{color:var(--muted);font-size:13px;line-height:1.5}.badge{display:inline-block;border-radius:999px;padding:6px 10px;background:rgba(255,255,255,.08);color:var(--accent2);font-size:12px}.card{border:1px solid var(--line);border-radius:10px;padding:10px;background:rgba(255,255,255,.05)}.stats{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin:10px 0}.value{font-size:18px;font-weight:700;margin-top:4px}.lab{font-size:12px;color:var(--muted)}"
"button,input,select{font:inherit}button{border:0;border-radius:10px;padding:10px 12px;font-weight:700;cursor:pointer}.primary{color:#04111d;background:linear-gradient(135deg,var(--accent),var(--accent2))}.secondary{color:var(--ink);background:rgba(255,255,255,.09);border:1px solid var(--line)}.danger{color:#fff;background:linear-gradient(135deg,#ff7e79,#ff5d8f)}button:disabled{opacity:.55;cursor:wait}input,select{width:100%;padding:10px;border-radius:10px;border:1px solid var(--line);background:rgba(255,255,255,.06);color:var(--ink)}select option{color:#111}.stack{display:grid;gap:8px}.list{display:grid;gap:8px;max-height:220px;overflow:auto}.item{width:100%;text-align:left;border:1px solid transparent;border-radius:10px;padding:10px;background:rgba(255,255,255,.06);color:var(--ink)}.item.active{border-color:var(--accent);background:rgba(89,209,255,.14)}.preview{display:grid;grid-template-columns:1fr 1fr;gap:8px}.preview canvas{width:100%;display:block;background:#fff;border-radius:8px;image-rendering:pixelated}.tools{display:grid;gap:8px}.file-name{font-size:13px;color:var(--muted);word-break:break-all}.msg.success{color:var(--accent2)}.msg.error{color:var(--danger)}.section{margin-top:14px;padding-top:14px;border-top:1px dashed var(--line)}@media(max-width:860px){.shell,.stats,.preview{grid-template-columns:1fr}}"
"</style></head><body><div class='shell'><section class='panel'><div class='toolbar'><span class='badge'>Ink ESP | 2.13</span><button class='secondary' id='scanBtn' type='button'>&#8635; &#25195;&#25551;</button></div><h1>&#37197;&#32593;</h1><div class='stats'><div class='card'><div class='lab'>&#29366;&#24577;</div><div class='value' id='stateVal'>idle</div></div><div class='card'><div class='lab'>&#22320;&#22336;</div><div class='value' id='ipVal'>192.168.4.1</div></div></div><div class='stack'><div class='list' id='wifiList'><div class='item'>&#25195;&#25551;&#20013;...</div></div><input id='ssidInput' placeholder='WiFi &#21517;&#31216;'><input id='passwordInput' type='password' placeholder='&#23494;&#30721;'><button class='primary' id='connectBtn' type='button'>&#10003; &#20445;&#23384;&#24182;&#36830;&#25509;</button><div class='msg' id='wifiMsg'></div></div>"
"<div class='section'><h2>&#24405;&#38899;</h2><div class='row'><button class='primary' id='recordBtn' type='button'>&#9679; &#24405;&#38899;</button><a href='/api/record.wav' download><button class='secondary' type='button'>&#8595; &#19979;&#36733;</button></a></div><audio id='player' controls preload='none' style='width:100%;margin-top:10px'></audio><div class='msg' id='audioMsg'></div></div></section>"
"<section class='panel'><h2>&#21047;&#22270;</h2><div class='tools'><input id='imageFile' type='file' accept='image/*,.bmp'><div class='row'><button class='secondary' id='pickBtn' type='button'>&#9635; &#36873;&#22270;</button><span class='file-name' id='fileName'>&#26410;&#36873;&#25321;</span></div><label class='hint'>&#32553;&#25918;<input id='zoomRange' type='range' min='20' max='320' value='100'></label><select id='algoSelect'><option value='clean'>&#20928;&#30333;</option><option value='bayer'>&#22270;&#26631;</option><option value='fs'>&#29031;&#29255;</option><option value='fs_vivid'>&#21160;&#28459;&#22686;&#24378;</option><option value='fs_clean'>&#21160;&#28459;&#20928;&#30333;</option><option value='atkinson'>&#26580;&#21644;</option><option value='edge'>&#32447;&#26465;</option></select><select id='refreshSelect'><option value='tri'>&#19977;&#33394;&#20840;&#21047;</option><option value='bw'>&#40657;&#30333;&#24555;&#21047;</option></select><button class='secondary' id='previewBtn' type='button'>&#9680; &#39044;&#35272;</button><button class='primary' id='uploadBtn' type='button'>&#8593; &#21047;&#23631;</button><div class='msg' id='imageMsg'></div><div class='preview'><div><div class='lab'>&#35009;&#21098;</div><canvas id='cropCanvas' width='104' height='212'></canvas></div><div><div class='lab'>&#39044;&#35272;</div><canvas id='previewCanvas' width='104' height='212'></canvas></div></div></div>"
"<div class='section'><h2>&#26085;&#21382;</h2><button class='danger' id='calendarBtn' type='button'>&#9633; &#19977;&#26085;&#21382;</button><div class='msg' id='calendarMsg'></div></div><div class='section'><h2>&#22791;&#24536;&#24405;</h2><div class='stack'><div class='row'><input id='memo1' maxlength='31' placeholder='&#20107;&#39033; 1'><label class='hint'><input id='done1' type='checkbox' style='width:auto'> &#10003;</label></div><div class='row'><input id='memo2' maxlength='31' placeholder='&#20107;&#39033; 2'><label class='hint'><input id='done2' type='checkbox' style='width:auto'> &#10003;</label></div><div class='row'><input id='memo3' maxlength='31' placeholder='&#20107;&#39033; 3'><label class='hint'><input id='done3' type='checkbox' style='width:auto'> &#10003;</label></div><button class='primary' id='memoBtn' type='button'>&#9745; &#26174;&#31034;</button><div class='msg' id='memoMsg'></div></div></div></section></div>"
"<script>"
"var W=104,H=212,LEN=W*H/8,ORIGIN='http://192.168.4.1';function E(id){return document.getElementById(id)}function addEv(n,e,f){if(!n)return;if(n.addEventListener)n.addEventListener(e,f,false);else if(n.attachEvent)n.attachEvent('on'+e,f);else n['on'+e]=f}function msg(n,t,k){if(!n)return;n.innerHTML='';n.appendChild(document.createTextNode(t||''));n.className='msg'+(k?' '+k:'')}function req(m,u,b,type,ok,fail){var x=new XMLHttpRequest();x.open(m,ORIGIN+u,true);x.timeout=60000;if(type)x.setRequestHeader('Content-Type',type);x.onreadystatechange=function(){if(x.readyState!==4)return;var d={};try{d=x.responseText?JSON.parse(x.responseText):{}}catch(e){d={message:x.responseText||''}}if(x.status>=200&&x.status<300){if(ok)ok(d)}else{if(fail)fail(d.message||('HTTP '+x.status))}};x.onerror=function(){if(fail)fail('fail')};x.ontimeout=function(){if(fail)fail('timeout')};x.send(b||null)}"
"var wifiList=E('wifiList'),ssidInput=E('ssidInput'),passwordInput=E('passwordInput'),wifiMsg=E('wifiMsg'),stateVal=E('stateVal'),ipVal=E('ipVal'),imageFile=E('imageFile'),fileName=E('fileName'),imageMsg=E('imageMsg'),audioMsg=E('audioMsg'),calendarMsg=E('calendarMsg'),memoMsg=E('memoMsg'),cropCanvas=E('cropCanvas'),previewCanvas=E('previewCanvas'),cropCtx=cropCanvas.getContext('2d'),previewCtx=previewCanvas.getContext('2d'),img=null,frame=null,crop={x:0,y:0,drag:false,lx:0,ly:0};"
"function status(){req('GET','/api/status',null,null,function(s){stateVal.innerHTML=s.state||'-';ipVal.innerHTML=s.ip||'192.168.4.1';if(s.has_recording)E('player').src='/api/record.wav?t='+new Date().getTime()})}function scan(){msg(wifiMsg,'scan...');req('GET','/api/scan',null,null,function(d){var a=d.networks||[];wifiList.innerHTML='';for(var i=0;i<a.length;i++){(function(n){var b=document.createElement('button');b.type='button';b.className='item';b.appendChild(document.createTextNode(n.ssid+'  '+n.rssi+' dBm  '+n.auth));b.onclick=function(){ssidInput.value=n.ssid;var cs=wifiList.getElementsByTagName('button');for(var j=0;j<cs.length;j++)cs[j].className='item';b.className='item active'};wifiList.appendChild(b)})(a[i])}msg(wifiMsg,'ok','success')},function(e){msg(wifiMsg,e,'error')})}function connectWifi(){msg(wifiMsg,'connect...');req('POST','/api/connect',JSON.stringify({ssid:ssidInput.value,password:passwordInput.value}),'application/json',function(d){msg(wifiMsg,d.message||'ok','success');setTimeout(status,2000)},function(e){msg(wifiMsg,e,'error')})}"
"function clamp(v){return v<0?0:(v>255?255:v)}function luminance(r,g,b){return(r*30+g*59+b*11)/100}function orderedThreshold(x,y){var m=[0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5];return(m[(y&3)*4+(x&3)]-7.5)*10}function classifyPixel(r,g,b,x,y,algo,fast){var l=luminance(r,g,b),rs=r-Math.max(g,b),sat=Math.max(r,g,b)-Math.min(r,g,b);if(!fast&&r>=130&&rs>32&&sat>45&&l<235)return 2;if(algo==='bayer')return l+orderedThreshold(x,y)<138?1:0;if(algo==='edge')return l<150?1:0;return l<118?1:0}function nearestPalette(r,g,b,fast,algo){var l=luminance(r,g,b),rs=r-Math.max(g,b),sat=Math.max(r,g,b)-Math.min(r,g,b),rg=120,rb=28,bg=145;if(algo==='fs_vivid'){rg=132;rb=38;bg=152;r=clamp((r-128)*1.08+128);g=clamp((g-128)*1.03+128);b=clamp((b-128)*1.03+128);l=luminance(r,g,b);rs=r-Math.max(g,b);sat=Math.max(r,g,b)-Math.min(r,g,b)}if(algo==='fs_clean'){rg=136;rb=42;bg=138;if(l>182&&sat<52)return[255,255,255,0]}if(!fast&&r>=rg&&rs>rb&&sat>36&&l<238)return[255,0,0,2];return l<bg?[0,0,0,1]:[255,255,255,0]}function diffuse(d,w,h,x,y,er,eg,eb,wt,div){if(x<0||x>=w||y<0||y>=h)return;var i=(y*w+x)*4;d[i]=clamp(d[i]+er*wt/div);d[i+1]=clamp(d[i+1]+eg*wt/div);d[i+2]=clamp(d[i+2]+eb*wt/div)}function errorDiffuse(d,w,h,algo,fast){for(var y=0;y<h;y++){for(var x=0;x<w;x++){var i=(y*w+x)*4,or=d[i],og=d[i+1],ob=d[i+2],q=nearestPalette(or,og,ob,fast,algo);d[i]=q[0];d[i+1]=q[1];d[i+2]=q[2];d[i+3]=255;var er=or-q[0],eg=og-q[1],eb=ob-q[2];if(algo==='atkinson'){diffuse(d,w,h,x+1,y,er,eg,eb,1,8);diffuse(d,w,h,x+2,y,er,eg,eb,1,8);diffuse(d,w,h,x-1,y+1,er,eg,eb,1,8);diffuse(d,w,h,x,y+1,er,eg,eb,1,8);diffuse(d,w,h,x+1,y+1,er,eg,eb,1,8);diffuse(d,w,h,x,y+2,er,eg,eb,1,8)}else{diffuse(d,w,h,x+1,y,er,eg,eb,7,16);diffuse(d,w,h,x-1,y+1,er,eg,eb,3,16);diffuse(d,w,h,x,y+1,er,eg,eb,5,16);diffuse(d,w,h,x+1,y+1,er,eg,eb,1,16)}}}}"
"function setPix(buf,x,y,on){var i=((y*W+x)>>3),m=0x80>>(x&7);buf[i]=on?(buf[i]&~m):(buf[i]|m)}function getPix(buf,x,y){return(buf[((y*W+x)>>3)]&(0x80>>(x&7)))?0:1}function isEdge(g,x,y){if(x<1||y<1||x>=W-1||y>=H-1)return 0;var i=y*W+x,gx=-g[i-W-1]-2*g[i-1]-g[i+W-1]+g[i-W+1]+2*g[i+1]+g[i+W+1],gy=-g[i-W-1]-2*g[i-W]-g[i-W+1]+g[i+W-1]+2*g[i+W]+g[i+W+1];return(Math.abs(gx)+Math.abs(gy))>86?1:0}function denoisePlanes(bw,red){var sb=new Uint8Array(bw),sr=new Uint8Array(red);function gp(buf,x,y){return(buf[((y*W+x)>>3)]&(0x80>>(x&7)))?0:1}for(var y=1;y<H-1;y++){for(var x=1;x<W-1;x++){var bi=gp(sb,x,y),ri=gp(sr,x,y);if(!bi&&!ri)continue;var nb=0,nr=0;for(var dy=-1;dy<=1;dy++){for(var dx=-1;dx<=1;dx++){if(dx||dy){nb+=gp(sb,x+dx,y+dy);nr+=gp(sr,x+dx,y+dy)}}}if(bi&&nb<2)setPix(bw,x,y,0);if(ri&&nr<2)setPix(red,x,y,0)}}}function drawCrop(){cropCtx.fillStyle='#fff';cropCtx.fillRect(0,0,W,H);if(!img)return;var sc=Math.max(W/img.width,H/img.height)*(parseInt(E('zoomRange').value,10)/100),dw=img.width*sc,dh=img.height*sc;if(dw<W)crop.x=(W-dw)/2;else crop.x=Math.min(0,Math.max(W-dw,crop.x));if(dh<H)crop.y=(H-dh)/2;else crop.y=Math.min(0,Math.max(H-dh,crop.y));cropCtx.drawImage(img,crop.x,crop.y,dw,dh)}function buildPreview(){if(!img){msg(imageMsg,'pick image','error');return}drawCrop();var data=cropCtx.getImageData(0,0,W,H),p=data.data,bw=new Uint8Array(LEN),red=new Uint8Array(LEN),fast=E('refreshSelect').value==='bw',algo=E('algoSelect').value,gray=new Uint8Array(W*H),diffuseMode=(algo==='fs'||algo==='fs_vivid'||algo==='fs_clean'||algo==='atkinson');bw.fill(255);red.fill(255);if(diffuseMode)errorDiffuse(p,W,H,algo,fast);for(var gi=0;gi<W*H;gi++){var go=gi*4;gray[gi]=luminance(p[go],p[go+1],p[go+2])}for(var y=0;y<H;y++){for(var x=0;x<W;x++){var i=(y*W+x)*4,c;if(diffuseMode)c=(p[i]===255&&p[i+1]===0&&p[i+2]===0)?2:((p[i]===0&&p[i+1]===0&&p[i+2]===0)?1:0);else c=classifyPixel(p[i],p[i+1],p[i+2],x,y,algo,fast);if(algo==='edge'&&c===0&&isEdge(gray,x,y))c=1;if(c===2)setPix(red,x,y,1);else if(c===1)setPix(bw,x,y,1)}}if(algo==='clean'||algo==='fs_clean')denoisePlanes(bw,red);for(var yy=0;yy<H;yy++){for(var xx=0;xx<W;xx++){var o=(yy*W+xx)*4;if(getPix(red,xx,yy)){p[o]=255;p[o+1]=0;p[o+2]=0}else if(getPix(bw,xx,yy)){p[o]=0;p[o+1]=0;p[o+2]=0}else{p[o]=255;p[o+1]=255;p[o+2]=255}p[o+3]=255}}previewCtx.putImageData(data,0,0);frame=new Uint8Array(1+LEN*2);frame[0]=fast?1:0;frame.set(bw,1);frame.set(red,1+LEN);msg(imageMsg,'preview '+frame.length+'B','success')}"
"function uploadFrame(){if(!frame){msg(imageMsg,'preview first','error');return}msg(imageMsg,'probe...');req('POST','/api/upload/probe','probe','text/plain',function(){msg(imageMsg,'upload...');req('POST','/api/frame/upload',frame,'application/octet-stream',function(d){msg(imageMsg,d.message||'ok','success')},function(e){msg(imageMsg,e,'error')})},function(e){msg(imageMsg,e,'error')})}function pickChanged(){var f=imageFile.files&&imageFile.files[0];frame=null;if(!f){fileName.innerHTML='-';return}fileName.innerHTML=f.name;var r=new FileReader();r.onload=function(){var im=new Image();im.onload=function(){img=im;crop.x=0;crop.y=0;drawCrop();msg(imageMsg,'loaded','success')};im.src=r.result};r.readAsDataURL(f)}"
"function point(ev){var r=cropCanvas.getBoundingClientRect(),e=ev.touches&&ev.touches.length?ev.touches[0]:ev;return{x:(e.clientX-r.left)*W/r.width,y:(e.clientY-r.top)*H/r.height}}function down(ev){if(!img)return;if(ev.preventDefault)ev.preventDefault();var p=point(ev);crop.drag=true;crop.lx=p.x;crop.ly=p.y}function move(ev){if(!crop.drag||!img)return;if(ev.preventDefault)ev.preventDefault();var p=point(ev);crop.x+=p.x-crop.lx;crop.y+=p.y-crop.ly;crop.lx=p.x;crop.ly=p.y;frame=null;drawCrop()}function up(){crop.drag=false}function rec(){msg(audioMsg,'record...');req('POST','/api/record',null,null,function(d){msg(audioMsg,d.message||'ok','success');setTimeout(status,1200)},function(e){msg(audioMsg,e,'error')})}function cal(){msg(calendarMsg,'draw...');req('POST','/api/calendar',JSON.stringify({timestamp:Math.floor(new Date().getTime()/1000)}),'application/json',function(d){msg(calendarMsg,d.message||'ok','success')},function(e){msg(calendarMsg,e,'error')})}function memo(){var items=[],i,t;for(i=1;i<=3;i++){t=E('memo'+i).value;if(t){items.push({text:t,checked:E('done'+i).checked})}}if(!items.length){msg(memoMsg,'empty','error');return}msg(memoMsg,'draw...');req('POST','/api/memo',JSON.stringify({items:items}),'application/json',function(d){msg(memoMsg,d.message||'ok','success')},function(e){msg(memoMsg,e,'error')})}"
"addEv(E('scanBtn'),'click',scan);addEv(E('connectBtn'),'click',connectWifi);addEv(E('pickBtn'),'click',function(){try{imageFile.click()}catch(e){}});addEv(imageFile,'change',pickChanged);addEv(E('previewBtn'),'click',buildPreview);addEv(E('uploadBtn'),'click',uploadFrame);addEv(E('recordBtn'),'click',rec);addEv(E('calendarBtn'),'click',cal);addEv(E('memoBtn'),'click',memo);addEv(cropCanvas,'mousedown',down);addEv(cropCanvas,'mousemove',move);addEv(cropCanvas,'mouseup',up);addEv(cropCanvas,'mouseleave',up);addEv(cropCanvas,'touchstart',down);addEv(cropCanvas,'touchmove',move);addEv(cropCanvas,'touchend',up);addEv(E('zoomRange'),'input',function(){frame=null;drawCrop()});status();scan();setInterval(status,5000);"
"</script></body></html>";
static void portal_copy_str(char *dst, const char *src, size_t dst_size)
{
    size_t len;

    if ((dst == NULL) || (dst_size == 0)) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *portal_state_name(portal_state_t state)
{
    switch (state) {
        case PORTAL_STATE_PROVISIONING:
            return "provisioning";
        case PORTAL_STATE_CONNECTING:
            return "connecting";
        case PORTAL_STATE_CONNECTED:
            return "connected";
        case PORTAL_STATE_ERROR:
            return "error";
        case PORTAL_STATE_IDLE:
        default:
            return "idle";
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    esp_err_t err;

    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static esp_err_t send_message(httpd_req_t *req, int status_code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    esp_err_t err;

    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (status_code != 200) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    cJSON_AddStringToObject(root, "message", message);
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int remaining = req->content_len;
    int offset = 0;

    ESP_RETURN_ON_FALSE(buf != NULL, ESP_ERR_INVALID_ARG, TAG, "body buffer null");
    ESP_RETURN_ON_FALSE(buf_len > 0, ESP_ERR_INVALID_ARG, TAG, "body buffer empty");
    ESP_RETURN_ON_FALSE((size_t)remaining < buf_len, ESP_ERR_INVALID_SIZE, TAG, "body too large");

    while (remaining > 0) {
        int got = httpd_req_recv(req, buf + offset, remaining);
        if (got <= 0) {
            return ESP_FAIL;
        }
        remaining -= got;
        offset += got;
    }
    buf[offset] = '\0';
    return ESP_OK;
}

static void portal_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    if (nvs_open(PORTAL_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_str(handle, PORTAL_KEY_SSID, ssid);
    nvs_set_str(handle, PORTAL_KEY_PASSWORD, password);
    nvs_commit(handle);
    nvs_close(handle);
}

static bool portal_time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm now_tm = { 0 };

    localtime_r(&now, &now_tm);
    return (now_tm.tm_year + 1900) >= 2024;
}

static void portal_start_sntp_once(void)
{
    if (s_portal.sntp_started) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(TAG, "[TIME] SNTP init failed: %s", esp_err_to_name(err));
        return;
    }

    s_portal.sntp_started = true;
    ESP_LOGI(TAG, "[TIME] SNTP started");
}

static uint32_t seconds_until_next_midnight(void)
{
    time_t now = time(NULL);
    struct tm next_tm = { 0 };
    time_t next_time;
    int seconds;

    localtime_r(&now, &next_tm);
    next_tm.tm_sec = 0;
    next_tm.tm_min = 0;
    next_tm.tm_hour = 0;
    next_tm.tm_mday += 1;
    next_time = mktime(&next_tm);
    seconds = (int)difftime(next_time, now);
    if (seconds < 30) {
        seconds = 30;
    }
    return (uint32_t)seconds;
}

static void calendar_auto_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!s_portal.sta_connected || !s_portal.sntp_started) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (!portal_time_is_valid()) {
            ESP_LOGI(TAG, "[TIME] Waiting for SNTP time sync");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        uint32_t wait_seconds = seconds_until_next_midnight();
        ESP_LOGI(TAG, "[CAL] Next auto refresh in %lu seconds", (unsigned long)wait_seconds);
        vTaskDelay(pdMS_TO_TICKS(wait_seconds * 1000UL));

        if (s_portal.sta_connected && portal_time_is_valid()) {
            time_t now = time(NULL);
            esp_err_t err = eink_scene_show_calendar(now);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "[CAL] Auto calendar refreshed at local midnight");
            } else {
                ESP_LOGW(TAG, "[CAL] Auto calendar refresh failed: %s", esp_err_to_name(err));
            }
        }
    }
}

static void portal_start_calendar_task_once(void)
{
    if (s_portal.calendar_task_started) {
        return;
    }
    if (xTaskCreate(calendar_auto_task, "calendar_auto", 4096, NULL, 2, &s_portal.calendar_task) == pdPASS) {
        s_portal.calendar_task_started = true;
    } else {
        ESP_LOGW(TAG, "[CAL] Failed to create auto calendar task");
    }
}

static void portal_load_credentials(void)
{
    nvs_handle_t handle;
    size_t ssid_len = sizeof(s_portal.ssid);
    size_t pass_len = sizeof(s_portal.password);

    s_portal.credentials_loaded = false;
    if (nvs_open(PORTAL_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    if ((nvs_get_str(handle, PORTAL_KEY_SSID, s_portal.ssid, &ssid_len) == ESP_OK) &&
        (nvs_get_str(handle, PORTAL_KEY_PASSWORD, s_portal.password, &pass_len) == ESP_OK) &&
        (s_portal.ssid[0] != '\0')) {
        s_portal.credentials_loaded = true;
    }
    nvs_close(handle);
}

static esp_err_t portal_connect_sta(const char *ssid, const char *password)
{
    wifi_config_t sta_cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
        },
    };

    portal_copy_str((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    portal_copy_str((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set sta config failed");
    esp_wifi_disconnect();
    s_portal.state = PORTAL_STATE_CONNECTING;
    return esp_wifi_connect();
}

static void portal_autoconnect_task(void *arg)
{
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (CONFIG_AUDIO_PORTAL_STA_AUTOCONNECT_DELAY_MS > 0) {
            ESP_LOGI(TAG, "[WiFi] Saved STA autoconnect waits %d ms; AP stays discoverable first",
                CONFIG_AUDIO_PORTAL_STA_AUTOCONNECT_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_AUDIO_PORTAL_STA_AUTOCONNECT_DELAY_MS));
        }

        portal_load_credentials();
        if (!s_portal.credentials_loaded) {
            ESP_LOGI(TAG, "[WiFi] No saved STA credentials; stay in AP portal mode");
            s_portal.autoconnect_requested = false;
            continue;
        }
        if (s_portal.sta_connected) {
            s_portal.autoconnect_requested = false;
            continue;
        }

        ESP_LOGI(TAG, "[WiFi] Auto-connecting saved STA SSID: %s", s_portal.ssid);
        esp_err_t err = portal_connect_sta(s_portal.ssid, s_portal.password);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[WiFi] Saved STA autoconnect failed to start: %s", esp_err_to_name(err));
        }
        s_portal.autoconnect_requested = false;
    }
}

static void portal_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if ((base == WIFI_EVENT) && (id == WIFI_EVENT_STA_DISCONNECTED)) {
        ESP_LOGW(TAG, "[WiFi] STA disconnected");
        s_portal.sta_connected = false;
        s_portal.ip_address[0] = '\0';
        s_portal.state = s_portal.portal_enabled ? PORTAL_STATE_PROVISIONING : PORTAL_STATE_IDLE;
        return;
    }
    if ((base == IP_EVENT) && (id == IP_EVENT_STA_GOT_IP)) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        s_portal.sta_connected = true;
        s_portal.state = PORTAL_STATE_CONNECTED;
        if (event != NULL) {
            snprintf(s_portal.ip_address, sizeof(s_portal.ip_address), IPSTR, IP2STR(&event->ip_info.ip));
        }
        ESP_LOGI(TAG, "[WiFi] STA connected: http://%s/", s_portal.ip_address);
        portal_start_sntp_once();
        portal_start_calendar_task_once();
    }
}

static esp_err_t portal_wifi_init(void)
{
    esp_err_t err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_portal.wifi_initialized) {
        return ESP_OK;
    }

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    s_portal.ap_netif = esp_netif_create_default_wifi_ap();
    s_portal.sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE((s_portal.ap_netif != NULL) && (s_portal.sta_netif != NULL), ESP_FAIL, TAG, "netif failed");

    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, portal_event_handler, NULL), TAG, "wifi handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, portal_event_handler, NULL), TAG, "ip handler failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi sta mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    xTaskCreate(portal_autoconnect_task, "portal_auto_sta", 4096, NULL, 3, &s_portal.autoconnect_task);

    s_portal.wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] GET /");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    ESP_LOGD(TAG, "[HTTP] GET /api/status");
    cJSON *root = cJSON_CreateObject();
    recording_status_t rec = { 0 };
    esp_err_t err;

    recording_service_get_status(&rec);
    cJSON_AddStringToObject(root, "state", portal_state_name(s_portal.state));
    cJSON_AddStringToObject(root, "ssid", s_portal.sta_connected ? s_portal.ssid : "");
    cJSON_AddStringToObject(root, "ip", s_portal.ip_address);
    cJSON_AddBoolToObject(root, "recording", rec.recording);
    cJSON_AddBoolToObject(root, "has_recording", rec.has_recording);
    cJSON_AddNumberToObject(root, "file_size", (double)rec.file_size);
    cJSON_AddNumberToObject(root, "duration_ms", rec.duration_ms);
    cJSON_AddNumberToObject(root, "sample_rate_hz", rec.sample_rate_hz);
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static const char *auth_name(wifi_auth_mode_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa/wpa2";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3";
        default:
            return "secure";
    }
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] GET /api/scan");
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    wifi_ap_record_t records[PORTAL_MAX_SCAN_RESULTS] = { 0 };
    uint16_t count = PORTAL_MAX_SCAN_RESULTS;
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    esp_err_t err;

    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_cfg, true), TAG, "scan failed");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records), TAG, "scan records failed");

    for (uint16_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddStringToObject(item, "auth", auth_name(records[i].authmode));
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddItemToObject(root, "networks", list);
    err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t connect_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] POST /api/connect");
    char body[256];
    cJSON *root;
    const cJSON *ssid;
    const cJSON *password;

    ESP_RETURN_ON_ERROR(read_body(req, body, sizeof(body)), TAG, "read connect body failed");
    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "connect json failed");

    ssid = cJSON_GetObjectItem(root, "ssid");
    password = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(ssid) || (ssid->valuestring[0] == '\0')) {
        cJSON_Delete(root);
        return send_message(req, 400, "SSID is required");
    }

    portal_copy_str(s_portal.ssid, ssid->valuestring, sizeof(s_portal.ssid));
    portal_copy_str(s_portal.password, cJSON_IsString(password) ? password->valuestring : "", sizeof(s_portal.password));
    portal_save_credentials(s_portal.ssid, s_portal.password);
    s_portal.credentials_loaded = true;
    cJSON_Delete(root);

    ESP_RETURN_ON_ERROR(portal_connect_sta(s_portal.ssid, s_portal.password), TAG, "connect sta failed");
    return send_message(req, 200, "Wi-Fi connection started");
}

static esp_err_t frame_upload_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] POST /api/frame/upload len=%d", req->content_len);
    size_t offset = 0;
    int remaining = req->content_len;
    uint8_t mode;
    uint8_t *bw;
    uint8_t *red;

    if (req->content_len != PORTAL_FRAME_UPLOAD_LEN) {
        return send_message(req, 400, "Frame length must match 2.13 inch panel");
    }

    while (remaining > 0) {
        int got = httpd_req_recv(req, (char *)s_upload_buffer + offset, remaining);
        if (got <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)got;
        remaining -= got;
    }

    mode = s_upload_buffer[0];
    bw = &s_upload_buffer[1];
    red = &s_upload_buffer[1 + EINK_PANEL_BUF_LEN];
    if (mode == PORTAL_FRAME_MODE_BW_FAST) {
        memset(red, 0xFF, EINK_PANEL_BUF_LEN);
    } else if (mode != PORTAL_FRAME_MODE_TRI) {
        return send_message(req, 400, "Unknown refresh mode");
    }

    ESP_RETURN_ON_ERROR(eink_scene_show_frame(bw, red), TAG, "show uploaded frame failed");
    ESP_LOGI(TAG, "[OK] Uploaded frame refreshed: mode=%u", mode);
    return send_message(req, 200, "E-paper frame refreshed");
}

static esp_err_t upload_probe_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] POST /api/upload/probe len=%d", req->content_len);

    if (req->content_len > 0) {
        char discard[16];
        int remaining = req->content_len;

        while (remaining > 0) {
            int chunk = remaining > (int)sizeof(discard) ? (int)sizeof(discard) : remaining;
            int got = httpd_req_recv(req, discard, chunk);
            if (got <= 0) {
                return ESP_FAIL;
            }
            remaining -= got;
        }
    }

    return send_message(req, 200, "Upload probe reached Ink ESP");
}

static esp_err_t calendar_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] POST /api/calendar");
    char body[96] = { 0 };
    time_t timestamp = time(NULL);

    if (req->content_len > 0) {
        cJSON *root;
        if (read_body(req, body, sizeof(body)) == ESP_OK) {
            root = cJSON_Parse(body);
            if (root != NULL) {
                const cJSON *item = cJSON_GetObjectItem(root, "timestamp");
                if (cJSON_IsNumber(item)) {
                    timestamp = (time_t)item->valuedouble;
                }
                cJSON_Delete(root);
            }
        }
    }

    ESP_RETURN_ON_ERROR(eink_scene_show_calendar(timestamp), TAG, "show calendar failed");
    ESP_LOGI(TAG, "[OK] 3-day calendar refreshed");
    return send_message(req, 200, "Calendar refreshed");
}

static esp_err_t memo_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] POST /api/memo");
    char body[512];
    cJSON *root;
    const cJSON *items;
    eink_memo_item_t memos[EINK_MEMO_MAX_ITEMS] = { 0 };
    size_t count = 0;

    ESP_RETURN_ON_ERROR(read_body(req, body, sizeof(body)), TAG, "read memo body failed");
    root = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "memo json failed");

    items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return send_message(req, 400, "items is required");
    }

    for (int i = 0; (i < cJSON_GetArraySize(items)) && (count < EINK_MEMO_MAX_ITEMS); ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        const cJSON *text = cJSON_GetObjectItem(item, "text");
        const cJSON *checked = cJSON_GetObjectItem(item, "checked");

        if (!cJSON_IsString(text) || (text->valuestring[0] == '\0')) {
            continue;
        }
        portal_copy_str(memos[count].text, text->valuestring, sizeof(memos[count].text));
        memos[count].checked = cJSON_IsTrue(checked);
        ++count;
    }
    cJSON_Delete(root);

    if (count == 0) {
        return send_message(req, 400, "memo is empty");
    }

    ESP_RETURN_ON_ERROR(eink_scene_show_memos(memos, count), TAG, "show memos failed");
    ESP_LOGI(TAG, "[OK] Memo refreshed: %u items", (unsigned int)count);
    return send_message(req, 200, "Memo refreshed");
}

static esp_err_t trigger_record_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "[HTTP] POST /api/record");
    esp_err_t err = recording_service_trigger();
    if (err != ESP_OK) {
        return send_message(req, 400, "Recording unavailable or already running");
    }
    return send_message(req, 200, "Recording started");
}

static esp_err_t download_record_handler(httpd_req_t *req)
{
    const char *path = recording_service_get_file_path();
    struct stat st;
    FILE *file;
    char buffer[1024];
    size_t read_bytes;

    if (stat(path, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "recording not found");
        return ESP_FAIL;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to open recording");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"record.wav\"");
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK) {
            fclose(file);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t portal_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    if (s_portal.server != NULL) {
        return ESP_OK;
    }

    config.max_uri_handlers = 16;
    config.max_resp_headers = 12;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_portal.server, &config), TAG, "httpd_start failed");

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler },
        { .uri = "/api/connect", .method = HTTP_POST, .handler = connect_handler },
        { .uri = "/api/upload/probe", .method = HTTP_POST, .handler = upload_probe_handler },
        { .uri = "/api/frame/upload", .method = HTTP_POST, .handler = frame_upload_handler },
        { .uri = "/api/calendar", .method = HTTP_POST, .handler = calendar_handler },
        { .uri = "/api/memo", .method = HTTP_POST, .handler = memo_handler },
        { .uri = "/api/record", .method = HTTP_POST, .handler = trigger_record_handler },
        { .uri = "/api/record.wav", .method = HTTP_GET, .handler = download_record_handler },
        { .uri = "/generate_204", .method = HTTP_GET, .handler = redirect_handler },
        { .uri = "/gen_204", .method = HTTP_GET, .handler = redirect_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = redirect_handler },
        { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = redirect_handler },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_portal.server, &uris[i]), TAG, "register uri failed");
    }

    ESP_LOGI(TAG, "[OK] HTTP portal ready");
    return ESP_OK;
}

static esp_err_t portal_enable_ap(void)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = PORTAL_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    portal_copy_str((char *)ap_cfg.ap.ssid, CONFIG_AUDIO_PORTAL_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(CONFIG_AUDIO_PORTAL_AP_SSID);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config failed");
    s_portal.portal_enabled = true;
    if (!s_portal.sta_connected) {
        s_portal.state = PORTAL_STATE_PROVISIONING;
    }
    ESP_LOGI(TAG, "[OK] SoftAP enabled: ssid=%s url=http://192.168.4.1/", CONFIG_AUDIO_PORTAL_AP_SSID);
    return ESP_OK;
}

esp_err_t portal_service_start_minimal_wifi(void)
{
    return portal_service_start();
}

esp_err_t portal_service_start(void)
{
    ESP_RETURN_ON_ERROR(portal_wifi_init(), TAG, "wifi init failed");
    if (CONFIG_AUDIO_PORTAL_WIFI_START_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_AUDIO_PORTAL_WIFI_START_DELAY_MS));
    }
    ESP_RETURN_ON_ERROR(portal_enable_ap(), TAG, "enable AP failed");
    ESP_RETURN_ON_ERROR(portal_start_http_server(), TAG, "http server failed");
    if ((s_portal.autoconnect_task != NULL) && !s_portal.autoconnect_requested) {
        s_portal.autoconnect_requested = true;
        xTaskNotifyGive(s_portal.autoconnect_task);
    }
    ESP_LOGI(TAG, "[OK] Portal AP is ready; saved STA autoconnect is delayed");
    return ESP_OK;
}

esp_err_t portal_service_stop(void)
{
    ESP_RETURN_ON_FALSE(s_portal.wifi_initialized, ESP_ERR_INVALID_STATE, TAG, "wifi not initialized");

    if (s_portal.server != NULL) {
        ESP_RETURN_ON_ERROR(httpd_stop(s_portal.server), TAG, "httpd_stop failed");
        s_portal.server = NULL;
    }

    if (s_portal.sta_connected) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
        s_portal.state = PORTAL_STATE_CONNECTED;
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "disable AP failed");
        s_portal.state = PORTAL_STATE_IDLE;
    }

    s_portal.portal_enabled = false;
    ESP_LOGI(TAG, "[OK] Portal AP stopped");
    return ESP_OK;
}

esp_err_t portal_service_toggle(void)
{
    if (s_portal.portal_enabled) {
        return portal_service_stop();
    }
    return portal_service_start();
}

esp_err_t portal_service_init(void)
{
    memset(&s_portal, 0, sizeof(s_portal));
    s_portal.state = PORTAL_STATE_IDLE;
    ESP_LOGI(TAG, "[OK] Portal armed; hold BOOT for %d ms to start AP/STA console",
        CONFIG_AUDIO_PORTAL_BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}

bool portal_service_is_running(void)
{
    return s_portal.portal_enabled;
}


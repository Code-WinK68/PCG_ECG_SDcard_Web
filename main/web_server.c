#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "web_server.h"

static const char *TAG = "WEB";
static httpd_handle_t s_http_server = NULL;
static web_session_info_t s_session;
static bool s_files_deleted = false;

typedef struct {
    int32_t pcg_min;
    int32_t pcg_max;
    int64_t ecg_sum;
    uint16_t ecg_count;
    bool pcg_valid;
} preview_bucket_t;

static const char s_index_html[] =
    "<!doctype html>"
    "<html lang=\"vi\">"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ECG + PCG Recorder</title>"
    "<style>"
    ":root{--blue:#3656da;--blue2:#4b39d8;--line:#d9e0ea;--text:#1d2738;--muted:#6d7585;--green:#13a05b;--pcg:#2958db;--danger:#f23322}"
    "*{box-sizing:border-box} body{margin:0;background:#f7f8fb;color:var(--text);font-family:Arial,Helvetica,sans-serif;font-size:14px}.topbar{height:40px;background:linear-gradient(90deg,#1d63bd,#5036db);color:#fff;display:flex;align-items:center;padding:0 22px;font-size:18px;font-weight:700;letter-spacing:.2px}.shell{max-width:1220px;margin:0 auto;background:#fff;min-height:100vh;border-left:1px solid #e2e5ec;border-right:1px solid #e2e5ec}.head{height:68px;padding:0 26px;display:flex;align-items:center;border-bottom:1px solid #dfe4ed;gap:24px}.title{font-size:25px;font-weight:700;white-space:nowrap}.head-meta{display:flex;gap:22px;color:#535b68;font-weight:600;font-size:12px;flex:1}.nav{margin-left:auto;display:flex;gap:8px}.nav button,.tool button,.download{border:1px solid #d7dce5;border-radius:4px;background:#fff;padding:8px 17px;cursor:pointer;font-weight:700;color:#303746;box-shadow:0 1px 2px #0e173008}.nav button.active,.tool button.active{border-color:#6a56e6;background:linear-gradient(90deg,#3c55dc,#5941db);color:#fff}.page{display:none;padding:14px 18px 22px}.page.active{display:block}.data-layout{display:grid;grid-template-columns:205px minmax(0,1fr);gap:18px}.sidebar{border:1px solid #dfe5ed;border-radius:7px;background:#fbfcff;padding:14px 15px;height:max-content}.side-title{color:#3656da;font-size:16px;font-weight:700;margin:0 0 13px}.rows{display:grid;gap:9px;font-size:12px}.row{display:grid;grid-template-columns:68px 1fr;gap:5px}.row b{font-weight:500;color:#667080}.row span{word-break:break-word}.state{margin-top:22px;border:1px solid #ccebd6;background:#f1fbf4;border-radius:7px;padding:13px;color:#117840}.state strong{display:block;color:#0f9a50;font-size:16px;margin-bottom:10px}.state p{margin:6px 0;font-weight:600}.state.bad{background:#fff6f4;border-color:#ffd1ca;color:#b62a1d}.state.bad strong{color:#d63423}.content{min-width:0}.tool{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap;padding:1px 0 12px}.tool .left,.tool .right{display:flex;align-items:center;gap:7px;flex-wrap:wrap}.tool label{font-weight:700;color:#515a67;margin-left:7px}.tool input{accent-color:#4a47db;width:15px;height:15px;vertical-align:middle}.tool .zoom{padding-left:7px}.chart{position:relative;border:1px solid var(--line);border-radius:5px;margin:0 0 14px;background:#fff;padding:8px 10px 4px}.chart h3{font-size:14px;margin:0 0 4px;color:#283343}.chart canvas{display:block;width:100%;height:175px}.xlabel{text-align:center;color:#46515f;font-size:12px;font-weight:600;margin:2px 0 0}.actions{display:flex;gap:10px;margin-top:8px}.download{display:inline-flex;align-items:center;justify-content:center;text-decoration:none;color:#fff;background:linear-gradient(90deg,#3b56dd,#5941dc);border-color:#5e4be1;min-width:104px}.download.raw{color:#252b33;background:#fff;border-color:#d6dbe5}.download.danger{background:var(--danger);border-color:var(--danger)}.panel{border:1px solid #dfe5ed;border-radius:7px;padding:18px;background:#fbfcff;line-height:1.65}.panel h2{margin-top:0;color:#3656da}.hidden{display:none!important}.empty{text-align:center;padding:38px 10px;color:#657083}.tag{display:inline-block;border-radius:12px;padding:3px 9px;font-size:11px;background:#e7ecff;color:#4052c9;font-weight:700}@media(max-width:760px){.topbar{padding:0 13px;font-size:16px}.head{height:auto;padding:13px;flex-wrap:wrap}.head-meta{order:3;width:100%;gap:12px}.data-layout{grid-template-columns:1fr}.sidebar{display:grid;grid-template-columns:1fr 1fr;gap:12px}.state{margin-top:0}.chart canvas{height:155px}.title{font-size:21px}.nav{margin-left:0}.actions{flex-wrap:wrap}}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"topbar\">5. GIAO DIỆN WEB</div>"
    "<div class=\"shell\">"
    "<header class=\"head\">"
    "  <div class=\"title\">ECG + PCG Recorder</div>"
    "  <div class=\"head-meta\"><span>SSID: ECG-PCG-ESP32</span><span>IP: 192.168.4.1</span></div>"
    "  <nav class=\"nav\"><button data-page=\"home\">Home</button><button class=\"active\" data-page=\"data\">Data</button><button data-page=\"settings\">Settings</button></nav>"
    "</header>"
    "<section id=\"home\" class=\"page\"><div class=\"panel\"><h2>ECG + PCG Recorder</h2><p>Thiết bị chỉ mở Web sau khi phiên ghi đã đóng file an toàn, tạo CSV và kiểm tra CRC32.</p><p><span id=\"homeState\" class=\"tag\">Đang tải dữ liệu...</span></p><p>Chọn <b>Data</b> để xem đồ thị đồng bộ ECG–PCG và tải dữ liệu.</p></div></section>"
    "<section id=\"data\" class=\"page active\"><div class=\"data-layout\">"
    "  <aside class=\"sidebar\">"
    "    <h2 class=\"side-title\">Thông tin phiên</h2>"
    "    <div class=\"rows\" id=\"infoRows\"></div>"
    "    <div id=\"stateBox\" class=\"state\"><strong id=\"stateTitle\">Đang tải...</strong><p id=\"stateOne\">• Đang đọc metadata</p><p id=\"stateTwo\">• Đang đọc preview</p></div>"
    "  </aside>"
    "  <main class=\"content\">"
    "    <div class=\"tool\">"
    "      <div class=\"left\"><button id=\"timeMode\" class=\"active\">Xem theo thời gian</button><button id=\"sampleMode\">Xem theo mẫu</button></div>"
    "      <div class=\"right\"><label>Kênh:</label><label><input id=\"showECG\" type=\"checkbox\" checked> ECG</label><label><input id=\"showPCG\" type=\"checkbox\" checked> PCG</label><span class=\"zoom\"><label>Zoom:</label><button data-zoom=\"1\">1s</button><button data-zoom=\"5\">5s</button><button data-zoom=\"10\">10s</button><button data-zoom=\"all\" class=\"active\">All</button></span></div>"
    "    </div>"
    "    <div id=\"ecgChart\" class=\"chart\"><h3>ECG (400 Hz)</h3><canvas id=\"ecg\"></canvas><div id=\"ecgX\" class=\"xlabel\">Thời gian (s)</div></div>"
    "    <div id=\"pcgChart\" class=\"chart\"><h3>PCG (16 kHz)</h3><canvas id=\"pcg\"></canvas><div id=\"pcgX\" class=\"xlabel\">Thời gian (s)</div></div>"
    "    <div class=\"actions\"><a class=\"download\" href=\"/download\">Tải file CSV</a><a class=\"download raw\" href=\"/download/raw-pcg\">Tải file RAW</a><button id=\"deleteFile\" class=\"download danger\">Xóa file</button></div>"
    "  </main>"
    "</div></section>"
    "<section id=\"settings\" class=\"page\"><div class=\"panel\"><h2>Settings</h2><p><b>SoftAP:</b> ECG-PCG-ESP32 &nbsp; | &nbsp; <b>IP:</b> 192.168.4.1</p><p><b>PCG:</b> 16 kHz &nbsp; <b>ECG:</b> 400 Hz &nbsp; <b>Tỷ lệ đồng bộ:</b> 40 : 1.</p><p>Muốn thực hiện phiên ghi mới, reset ESP32. Không reset trong khi đang tải file.</p><button id=\"restartRecord\" class=\"download\">Ghi phiên mới</button></div></section>"
    "</div>"
    "<script>"
    "const $=q=>document.querySelector(q);let S=null,P=null,mode='time',zoom='all';"
    "function esc(v){return String(v == null ? '' : v).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}"
    "function setPage(id){document.querySelectorAll('.page').forEach(x=>x.classList.toggle('active',x.id===id));document.querySelectorAll('.nav button').forEach(x=>x.classList.toggle('active',x.dataset.page===id));}"
    "document.querySelectorAll('.nav button').forEach(b=>b.onclick=()=>setPage(b.dataset.page));"
    "function row(k,v){return `<div class=\"row\"><b>${esc(k)}:</b><span>${esc(v)}</span></div>`;}"
    "function canvas(id){const c=$(id),r=c.getBoundingClientRect(),d=devicePixelRatio||1;c.width=Math.max(1,Math.floor(r.width*d));c.height=Math.max(1,Math.floor(r.height*d));const x=c.getContext('2d');x.setTransform(d,0,0,d,0,0);return[c,x,r.width,r.height];}"
    "function range(){if(!P||!S)return{a:0,b:0};const n=P.pcg_min.length;let seconds=zoom==='all'?S.duration_s:Math.min(Number(zoom),S.duration_s);let count=Math.max(20,Math.round(n*seconds/S.duration_s));count=Math.min(n,count);return{a:n-count,b:n};}"
    "function axis(ctx,w,h,lo,hi,r,label){const L=48,R=10,T=6,B=22;ctx.strokeStyle='#e1e5ec';ctx.lineWidth=1;ctx.font='11px Arial';ctx.fillStyle='#536071';for(let i=0;i<5;i++){let y=T+(h-T-B)*i/4;let v=hi-(hi-lo)*i/4;ctx.beginPath();ctx.moveTo(L,y);ctx.lineTo(w-R,y);ctx.stroke();ctx.fillText(Math.round(v),3,y+4);}for(let i=0;i<5;i++){let x=L+(w-L-R)*i/4;ctx.beginPath();ctx.moveTo(x,T);ctx.lineTo(x,h-B);ctx.stroke();let p=r.a+(r.b-r.a)*i/4;let v=mode==='time'?p*S.duration_s/P.pcg_min.length:Math.round(p*S.total_pcg_samples/P.pcg_min.length);ctx.fillText(mode==='time'?Number(v).toFixed(v<10?2:1):String(v),x-9,h-6);}ctx.fillStyle='#596475';ctx.fillText(label,L+4,14);return{L,R,T,B};}"
    "function pcgScale(r){let lo=Infinity,hi=-Infinity;for(let i=r.a;i<r.b;i++){lo=Math.min(lo,P.pcg_min[i]);hi=Math.max(hi,P.pcg_max[i]);}if(!isFinite(lo)||lo===hi){lo=-1;hi=1;}let p=(hi-lo)*.07||1;return[lo-p,hi+p];}"
    "function drawPCG(){let box=$('#pcgChart');box.classList.toggle('hidden',!$('#showPCG').checked);if(box.classList.contains('hidden')||!P)return;const[c,x,w,h]=canvas('#pcg'),r=range(),[lo,hi]=pcgScale(r),a=axis(x,w,h,lo,hi,r,'biên độ');const pw=w-a.L-a.R,ph=h-a.T-a.B;x.strokeStyle='#315ad9';x.lineWidth=1;x.beginPath();for(let i=r.a;i<r.b;i++){let px=a.L+(i-r.a)*pw/Math.max(1,r.b-r.a-1);let y0=a.T+(hi-P.pcg_min[i])*ph/(hi-lo),y1=a.T+(hi-P.pcg_max[i])*ph/(hi-lo);x.moveTo(px,y0);x.lineTo(px,y1);}x.stroke();$('#pcgX').textContent=mode==='time'?'Thời gian (s)':'Chỉ số mẫu PCG';}"
    "function drawECG(){let box=$('#ecgChart');box.classList.toggle('hidden',!$('#showECG').checked);if(box.classList.contains('hidden')||!P)return;const[c,x,w,h]=canvas('#ecg'),r=range(),v=[];for(let i=r.a;i<r.b;i++)if(P.ecg[i]>=0)v.push(P.ecg[i]);if(!v.length){x.fillStyle='#657083';x.font='13px Arial';x.fillText('Không có ECG hợp lệ trong vùng hiển thị',16,25);return;}let lo=Math.min(...v),hi=Math.max(...v);if(lo===hi){lo-=1;hi+=1;}let pad=(hi-lo)*.07||1;lo-=pad;hi+=pad;const a=axis(x,w,h,lo,hi,r,'ADC raw'),pw=w-a.L-a.R,ph=h-a.T-a.B;x.strokeStyle='#17a45d';x.lineWidth=1.5;x.beginPath();let open=false;for(let i=r.a;i<r.b;i++){let q=P.ecg[i];if(q<0){open=false;continue;}let px=a.L+(i-r.a)*pw/Math.max(1,r.b-r.a-1);let py=a.T+(hi-q)*ph/(hi-lo);if(!open){x.moveTo(px,py);open=true;}else{x.lineTo(px,py);}}x.stroke();$('#ecgX').textContent=mode==='time'?'Thời gian (s)':'Chỉ số mẫu PCG';}"
    "function draw(){drawECG();drawPCG();}"
    "function renderInfo(){const status=S.deleted?'Đã xóa file':S.capture_valid?'Record complete':'Data integrity FAIL';$('#homeState').textContent=status;$('#infoRows').innerHTML=row('File',S.file.replace('/sdcard/',''))+row('Thời gian',S.duration_s+' s')+row('Bắt đầu','Không có RTC/NTP')+row('PCG Fs',S.pcg_rate+' Hz')+row('ECG Fs',S.ecg_rate+' Hz')+row('Tỷ lệ','40 : 1')+row('PCG mẫu',S.total_pcg_samples)+row('ECG mẫu',S.ecg_samples)+row('CRC32',S.crc32)+row('SD Errors',S.sd_errors)+row('Lead-off',S.ecg_lead_off)+row('LO+',S.ecg_lo_plus)+row('LO-',S.ecg_lo_minus);$('#stateBox').classList.toggle('bad',!S.capture_valid||S.deleted);$('#stateTitle').textContent=S.deleted?'Không còn dữ liệu':S.capture_valid?'Trạng thái':'Trạng thái lỗi';$('#stateOne').textContent=S.deleted?'• File CSV và RAW đã được xóa':S.capture_valid?'• Record complete':'• Có mất/thiếu dữ liệu';$('#stateTwo').textContent=S.deleted?'• Ghi phiên mới bằng Settings':S.capture_valid?'• Data ready':'• Không dùng để phân tích y sinh';document.querySelectorAll('#data .download').forEach(e=>e.classList.toggle('hidden',S.deleted));if(S.deleted){$('#ecgChart').classList.add('hidden');$('#pcgChart').classList.add('hidden');}}"
    "async function load(){try{S=await (await fetch('/api/summary')).json();renderInfo();if(S.deleted)return;P=await (await fetch('/api/preview?points=1600')).json();draw();}catch(e){$('#stateTitle').textContent='Không thể tải dữ liệu';$('#stateOne').textContent='• '+e;}}"
    "$('#timeMode').onclick=()=>{mode='time';$('#timeMode').classList.add('active');$('#sampleMode').classList.remove('active');draw();};$('#sampleMode').onclick=()=>{mode='sample';$('#sampleMode').classList.add('active');$('#timeMode').classList.remove('active');draw();};document.querySelectorAll('[data-zoom]').forEach(b=>b.onclick=()=>{zoom=b.dataset.zoom;document.querySelectorAll('[data-zoom]').forEach(x=>x.classList.toggle('active',x===b));draw();});$('#showECG').onchange=draw;$('#showPCG').onchange=draw;"
    "$('#deleteFile').onclick=async()=>{if(!confirm('Xóa CSV và hai file RAW của phiên hiện tại?'))return;let r=await fetch('/api/delete',{method:'POST'});if(!r.ok){alert('Không thể xóa file.');return;}await load();};$('#restartRecord').onclick=async()=>{if(!confirm('Reset ESP32 để ghi phiên mới?'))return;await fetch('/api/restart',{method:'POST'});$('#homeState').textContent='Thiết bị đang reset...';};addEventListener('resize',draw);load();"
    "</script>"
    "</body></html>"
    ;

static esp_err_t initialize_wifi_ap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            return ret;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "Cannot create default Wi-Fi AP netif");
        return ESP_FAIL;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    wifi_config_t ap_cfg = { 0 };
    memcpy(ap_cfg.ap.ssid, WEB_AP_SSID, strlen(WEB_AP_SSID));
    memcpy(ap_cfg.ap.password, WEB_AP_PASSWORD, strlen(WEB_AP_PASSWORD));
    ap_cfg.ap.ssid_len = strlen(WEB_AP_SSID);
    ap_cfg.ap.channel = WEB_AP_CHANNEL;
    ap_cfg.ap.max_connection = WEB_AP_MAX_CLIENTS;
    ap_cfg.ap.authmode = (strlen(WEB_AP_PASSWORD) >= 8U) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (ap_cfg.ap.authmode == WIFI_AUTH_OPEN) {
        ap_cfg.ap.password[0] = '\0';
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    ESP_LOGI(TAG, "SoftAP started: SSID=%s | password=%s | open http://192.168.4.1",
             WEB_AP_SSID, WEB_AP_PASSWORD);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t summary_get_handler(httpd_req_t *req)
{
    const double duration_s = (double)s_session.total_pcg_samples / (double)PCG_SAMPLE_RATE;
    char json[1024];
    const int n = snprintf(json, sizeof(json),
                           "{\"file\":\"%s\",\"duration_s\":%.3f,\"total_pcg_samples\":%" PRIu64 ","
                           "\"file_size_kb\":%.1f,\"crc32\":\"%s%08" PRIX32 "\","
                           "\"pcg_rate\":%u,\"ecg_rate\":%u,"
                           "\"pcg_blocks\":%" PRIu32 ",\"pcg_dropped\":%" PRIu32 ",\"pcg_short\":%" PRIu32 ","
                           "\"ecg_samples\":%" PRIu32 ",\"ecg_dropped\":%" PRIu32 ",\"ecg_missing\":%" PRIu32 ",\"sync_drop\":%" PRIu32 ","
                           "\"ecg_late\":%" PRIu32 ",\"pcg_raw_gaps\":%" PRIu32 ",\"pcg_raw_gap_samples\":%" PRIu64 ","
                           "\"capture_valid\":%s,"
                           "\"ecg_lead_off\":%" PRIu32 ",\"ecg_lo_plus\":%" PRIu32 ",\"ecg_lo_minus\":%" PRIu32 ","
                           "\"ecg_start_offset_us\":%" PRId64 ",\"sd_errors\":%" PRIu32 ",\"deleted\":%s}",
                           s_session.filename,
                           duration_s,
                           s_session.total_pcg_samples,
                           (double)s_session.file_size_bytes / 1024.0,
                           s_session.crc32_valid ? "0x" : "unavailable-0x",
                           s_session.file_crc32,
                           PCG_SAMPLE_RATE,
                           ECG_SAMPLE_RATE,
                           s_session.pcg_blocks_captured,
                           s_session.pcg_blocks_dropped,
                           s_session.pcg_short_blocks,
                           s_session.ecg_samples_captured,
                           s_session.ecg_samples_dropped,
                           s_session.ecg_samples_missing_in_merge,
                           s_session.sync_tokens_dropped,
                           s_session.ecg_late_notifications,
                           s_session.pcg_raw_frame_gaps,
                           s_session.pcg_raw_gap_samples,
                           s_session.capture_valid ? "true" : "false",
                           s_session.ecg_lead_off_samples,
                           s_session.ecg_lo_plus_samples,
                           s_session.ecg_lo_minus_samples,
                           s_session.ecg_first_offset_us,
                           s_session.sd_write_errors,
                           s_files_deleted ? "true" : "false");

    if (n < 0 || (size_t)n >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "summary JSON overflow");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n);
}

static size_t requested_preview_points(httpd_req_t *req)
{
    size_t points = WEB_PREVIEW_DEFAULT_POINTS;
    const size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0U || qlen >= 64U) {
        return points;
    }

    char query[64];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "points", value, sizeof(value)) == ESP_OK) {
        char *end = NULL;
        const unsigned long parsed = strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed > 20U && parsed <= WEB_PREVIEW_MAX_POINTS) {
            points = (size_t)parsed;
        }
    }
    return points;
}

static esp_err_t send_preview_json(httpd_req_t *req, const preview_bucket_t *buckets, size_t points)
{
    char chunk[96];
    esp_err_t ret = httpd_resp_send_chunk(req, "{\"pcg_min\":[", HTTPD_RESP_USE_STRLEN);
    if (ret != ESP_OK) return ret;
    for (size_t i = 0; i < points; ++i) {
        const int n = snprintf(chunk, sizeof(chunk), "%s%" PRId32,
                               i == 0U ? "" : ",",
                               buckets[i].pcg_valid ? buckets[i].pcg_min : 0);
        if (n < 0 || (size_t)n >= sizeof(chunk)) return ESP_FAIL;
        if ((ret = httpd_resp_send_chunk(req, chunk, n)) != ESP_OK) return ret;
    }

    if ((ret = httpd_resp_send_chunk(req, "],\"pcg_max\":[", HTTPD_RESP_USE_STRLEN)) != ESP_OK) return ret;
    for (size_t i = 0; i < points; ++i) {
        const int n = snprintf(chunk, sizeof(chunk), "%s%" PRId32,
                               i == 0U ? "" : ",",
                               buckets[i].pcg_valid ? buckets[i].pcg_max : 0);
        if (n < 0 || (size_t)n >= sizeof(chunk)) return ESP_FAIL;
        if ((ret = httpd_resp_send_chunk(req, chunk, n)) != ESP_OK) return ret;
    }

    if ((ret = httpd_resp_send_chunk(req, "],\"ecg\":[", HTTPD_RESP_USE_STRLEN)) != ESP_OK) return ret;
    for (size_t i = 0; i < points; ++i) {
        const int32_t ecg = buckets[i].ecg_count == 0U
                                ? -1
                                : (int32_t)(buckets[i].ecg_sum / buckets[i].ecg_count);
        const int n = snprintf(chunk, sizeof(chunk), "%s%" PRId32, i == 0U ? "" : ",", ecg);
        if (n < 0 || (size_t)n >= sizeof(chunk)) return ESP_FAIL;
        if ((ret = httpd_resp_send_chunk(req, chunk, n)) != ESP_OK) return ret;
    }

    if ((ret = httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN)) != ESP_OK) return ret;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t preview_get_handler(httpd_req_t *req)
{
    if (s_files_deleted) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session files deleted");
    }

    const size_t points = requested_preview_points(req);
    preview_bucket_t *buckets = calloc(points, sizeof(preview_bucket_t));
    if (buckets == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory for preview");
    }

    for (size_t i = 0; i < points; ++i) {
        buckets[i].pcg_min = INT32_MAX;
        buckets[i].pcg_max = INT32_MIN;
    }

    FILE *file = fopen(s_session.filename, "r");
    if (file == NULL) {
        free(buckets);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open CSV");
    }

    char line[128];
    (void)fgets(line, sizeof(line), file); /* CSV header */
    const uint64_t stride = (s_session.total_pcg_samples == 0U)
                                ? 1U
                                : (s_session.total_pcg_samples + points - 1U) / points;

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long sample_index = 0U;
        unsigned long long timestamp_us = 0U;
        int ecg = 0;
        int pcg = 0;
        unsigned int flags = 0U;

        if (sscanf(line, "%llu,%llu,%d,%d,%u",
                   &sample_index, &timestamp_us, &ecg, &pcg, &flags) != 5) {
            continue;
        }

        const uint64_t bucket_index = (uint64_t)sample_index / stride;
        if (bucket_index >= points) {
            continue;
        }

        preview_bucket_t *bucket = &buckets[bucket_index];
        if ((flags & CSV_FLAG_PCG_VALID) != 0U) {
            if (!bucket->pcg_valid || pcg < bucket->pcg_min) bucket->pcg_min = pcg;
            if (!bucket->pcg_valid || pcg > bucket->pcg_max) bucket->pcg_max = pcg;
            bucket->pcg_valid = true;
        }
        /* Do not draw ECG segments while AD8232 reports an open electrode.
         * The raw sample is still preserved in CSV for audit/debug purposes.
         */
        if ((flags & CSV_FLAG_ECG_VALID) != 0U &&
            (flags & CSV_FLAG_ECG_LEAD_OFF) == 0U) {
            bucket->ecg_sum += ecg;
            if (bucket->ecg_count < UINT16_MAX) {
                ++bucket->ecg_count;
            }
        }
    }

    fclose(file);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t ret = send_preview_json(req, buckets, points);
    free(buckets);
    return ret;
}

static esp_err_t send_download_file(httpd_req_t *req,
                                      const char *path,
                                      const char *mime,
                                      const char *attachment_name)
{
    if (s_files_deleted) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session files deleted");
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open file");
    }

    httpd_resp_set_type(req, mime);
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=%s", attachment_name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);

    char buffer[1024];
    esp_err_t ret = ESP_OK;
    while (true) {
        const size_t n = fread(buffer, 1U, sizeof(buffer), file);
        if (n > 0U) {
            ret = httpd_resp_send_chunk(req, buffer, n);
            if (ret != ESP_OK) {
                break;
            }
        }
        if (n < sizeof(buffer)) {
            if (ferror(file)) {
                ret = ESP_FAIL;
            }
            break;
        }
    }

    fclose(file);
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t download_get_handler(httpd_req_t *req)
{
    return send_download_file(req, s_session.filename, "text/csv", "ecg_pcg.csv");
}

static esp_err_t download_pcg_raw_get_handler(httpd_req_t *req)
{
    return send_download_file(req, s_session.pcg_raw_filename,
                              "application/octet-stream", "pcg_raw.bin");
}

static esp_err_t download_ecg_raw_get_handler(httpd_req_t *req)
{
    return send_download_file(req, s_session.ecg_raw_filename,
                              "application/octet-stream", "ecg_raw.bin");
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_restart();
}

static esp_err_t restart_post_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    if (xTaskCreate(restart_task, "web_restart", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Cannot create restart task");
    }
    return ESP_OK;
}

static esp_err_t delete_post_handler(httpd_req_t *req)
{
    if (!s_files_deleted) {
        const int csv_ret = remove(s_session.filename);
        const int pcg_ret = remove(s_session.pcg_raw_filename);
        const int ecg_ret = remove(s_session.ecg_raw_filename);

        if (csv_ret != 0 || pcg_ret != 0 || ecg_ret != 0) {
            ESP_LOGE(TAG, "Delete failed: csv=%d pcg=%d ecg=%d", csv_ret, pcg_ret, ecg_ret);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "cannot delete one or more session files");
        }

        s_files_deleted = true;
        s_session.file_size_bytes = 0U;
        s_session.capture_valid = false;
        s_session.crc32_valid = false;
        ESP_LOGI(TAG, "Session files deleted");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"deleted\"}");
}

static const httpd_uri_t s_root_uri = {
    .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_summary_uri = {
    .uri = "/api/summary", .method = HTTP_GET, .handler = summary_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_preview_uri = {
    .uri = "/api/preview", .method = HTTP_GET, .handler = preview_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_download_uri = {
    .uri = "/download", .method = HTTP_GET, .handler = download_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_download_pcg_raw_uri = {
    .uri = "/download/raw-pcg", .method = HTTP_GET, .handler = download_pcg_raw_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_download_ecg_raw_uri = {
    .uri = "/download/raw-ecg", .method = HTTP_GET, .handler = download_ecg_raw_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_restart_uri = {
    .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_delete_uri = {
    .uri = "/api/delete", .method = HTTP_POST, .handler = delete_post_handler, .user_ctx = NULL,
};

esp_err_t web_server_start(const web_session_info_t *session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_http_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(&s_session, session, sizeof(s_session));
    s_files_deleted = false;

    ESP_RETURN_ON_ERROR(initialize_wifi_ap(), TAG, "initialize_wifi_ap");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_uri_handlers = 9;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "httpd_start");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_root_uri), TAG, "register root");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_summary_uri), TAG, "register summary");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_preview_uri), TAG, "register preview");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_download_uri), TAG, "register download");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_download_pcg_raw_uri), TAG, "register raw PCG");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_download_ecg_raw_uri), TAG, "register raw ECG");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_restart_uri), TAG, "register restart");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &s_delete_uri), TAG, "register delete");

    ESP_LOGI(TAG, "Web server ready. Browse to http://192.168.4.1");
    return ESP_OK;
}

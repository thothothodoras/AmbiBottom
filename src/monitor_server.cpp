#include "monitor_server.hpp"
#include "settings_page.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace monitor_server {

namespace {

// ---- single self-contained HTML/CSS/JS page, no external assets --------
// Deliberately one file, embedded in the binary: this is a small status
// dashboard, not a large app, and shipping it as a literal here means
// there is nothing extra to copy alongside the binary onto the LXC.
// ---- single self-contained HTML/CSS/JS page, no external assets --------
// Ported from the old Python prototype's web/{index.html,style.css,app.js,
// color-model.js} (v1.5) - same four view modes, drawer, calibration and
// freshness charts - plus this bridge's own identity/pairing/systemd
// panel merged in. Deliberately one file, embedded in the binary: there is
// nothing extra to copy alongside ambi_bridge onto the LXC. Uses R"PAGE(...)
// rather than R"HTML(...) purely because the page text itself contains
// that exact HTML delimiter sequence in a few places.
const char* kPage = R"PAGE(<!doctype html>
<html lang="el">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#0a0f1c">
<title>Ambilight · Live Monitor</title>
<link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Cg transform='rotate(45 16 16)'%3E%3Crect x='5' y='5' width='10' height='10' rx='2.5' fill='%234d8eed'/%3E%3Crect x='17' y='5' width='10' height='10' rx='2.5' fill='%2372ded0'/%3E%3Crect x='5' y='17' width='10' height='10' rx='2.5' fill='%23e3a15e'/%3E%3Crect x='17' y='17' width='10' height='10' rx='2.5' fill='%239b67e7'/%3E%3C/g%3E%3C/svg%3E">
<style>
:root{color-scheme:dark;--bg:#0a0e17;--panel:#111725;--border:#263044;--muted:#8998b3;--txt:#e8edf6;--cyan:#53d1d4;--blue:#5388f5;--orange:#f2ae65;--green:#57d6a1}*{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 5% 5%,#172038 0%,transparent 48%),radial-gradient(ellipse at 95% 30%,#102138 0%,transparent 46%),var(--bg);font-family:Inter,ui-sans-serif,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:var(--txt);min-height:100vh}button,select,input{font:inherit}button{cursor:pointer}.shell{max-width:1350px;margin:auto;padding:30px 28px 20px}.topbar{display:flex;justify-content:space-between;gap:20px;align-items:center;margin-bottom:25px;flex-wrap:wrap}.brand{display:flex;align-items:center;gap:15px}.brand-mark{display:grid;grid-template-columns:repeat(2,12px);gap:4px;transform:rotate(45deg);width:30px;height:30px}.brand-mark span{border-radius:3px;background:#4d8eed}.brand-mark span:nth-child(2){background:#72ded0}.brand-mark span:nth-child(3){background:#e3a15e}.brand-mark span:nth-child(4){background:#9b67e7}.brand h1{font-size:19px;letter-spacing:.09em;font-weight:800;margin:0}.brand h1 em{font-style:normal;font-weight:400;color:#97a9c6}.brand p{font-size:12px;letter-spacing:.09em;color:#8d9bb4;margin:5px 0 0}.microdot{color:var(--cyan)}.top-actions{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.pill{display:inline-flex;align-items:center;gap:7px;background:#1b2937;border:1px solid #334354;border-radius:40px;padding:9px 13px;color:#c2d1e4;font-size:11px;font-weight:700;letter-spacing:.06em;white-space:nowrap}.pill.demo{color:#f3bd73;background:#3e3020;border-color:#765c32}.pill.offline .signal{background:#e0777c;box-shadow:0 0 6px #b94854}.pill.online .signal{background:#51e0ae;box-shadow:0 0 7px #4dcbaa}.signal{width:7px;height:7px;display:inline-block;border-radius:50%}.hidden{display:none!important}.workspace{display:grid;grid-template-columns:minmax(0,1.8fr) minmax(295px,1fr);gap:18px}.panel{background:linear-gradient(165deg,rgba(21,30,47,.97),rgba(13,20,33,.98));border:1px solid var(--border);border-radius:16px;box-shadow:0 12px 38px #02050b3a}.main-panel{padding:25px 27px 18px;min-width:0}.panel-top{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:22px}.eyebrow{font-weight:800;font-size:9px;letter-spacing:.17em;color:#6db0c7;margin-bottom:8px}.panel-top h2{font-size:20px;letter-spacing:-.025em;font-weight:670;margin:0}.subtle-label{font-size:11px;color:#8393ad}.sep{margin:0 3px;color:#506079}.mode-switch{display:flex;background:#0a1322;padding:4px;border-radius:9px;border:1px solid #263148;gap:3px;width:max-content;max-width:100%;margin-bottom:20px;flex-wrap:wrap}.mode-switch button{background:transparent;color:#8293aa;padding:9px 13px;border:0;border-radius:6px;font-size:12px;font-weight:700}.mode-switch button.selected{background:#243651;color:#dceeff;box-shadow:0 3px 10px #03071288}.tv-area{background:radial-gradient(ellipse at center,#172c44 0%,#0a1120 67%);padding:36px 36px 26px;border:1px solid #24304a;border-radius:13px;min-height:367px;display:flex;flex-direction:column;justify-content:center}.tv-assembly{width:100%;max-width:620px;margin:auto}.edge{display:flex;gap:3px}.edge-top,.edge-bottom{height:17px;margin:0 23px}.edge-top{margin-bottom:9px}.edge-bottom{margin-top:9px}.edge-vertical{width:15px;display:flex;flex-direction:column;gap:3px}.tv-centerline{display:flex;gap:8px;aspect-ratio:16/8.1;min-height:180px}.tv-body{position:relative;isolation:isolate;overflow:hidden;flex:1;border-radius:8px;border:2px solid #394256;background:radial-gradient(ellipse at 45% 50%,#25304a,#080e1c 70%);box-shadow:0 5px 21px #000a, inset 0 0 28px #000b}.screen-glow{position:absolute;inset:0;pointer-events:none;background:radial-gradient(ellipse at 45% 35%,#234c6b34,transparent 72%)}.tv-screen-content{display:flex;height:calc(100% - 25px);flex-direction:column;align-items:center;justify-content:center;gap:6px}.screen-kicker{font-size:10px;letter-spacing:.33em;color:#7b8da6}.screen-icon{font-size:37px;line-height:1;color:#55749a;text-shadow:0 0 25px #447aa273}.screen-main{font-size:16px;font-weight:700;color:#d4deea;letter-spacing:.02em}.screen-foot{font-size:9px;letter-spacing:.17em;color:#7587a2}.screen-bottom{display:flex;justify-content:space-between;gap:8px;padding:6px 10px;position:absolute;bottom:0;left:0;right:0;border-top:1px solid #2d3b4d;font-size:9px;letter-spacing:.07em;color:#8192ac}.edge .pixel{flex:1;min-width:0;background:#303e51;border-radius:3px;position:relative;box-shadow:0 0 0 transparent;transition:background-color .045s linear,box-shadow .045s linear}.edge .pixel.active{box-shadow:0 0 14px -2px var(--pixel-color),0 0 4px var(--pixel-color)}.edge-bottom .pixel.peak::after{content:'';position:absolute;left:50%;bottom:-8px;width:4px;height:4px;margin-left:-2px;border-radius:50%;background:#dfe8f5;opacity:.85}.edge-bottom .pixel.active{box-shadow:0 0 12px var(--pixel-color),0 4px 17px -2px var(--pixel-color)}.edge-legend{display:flex;justify-content:center;gap:24px;margin-top:23px;color:#9baac1;font-size:11px}.edge-legend span{display:flex;align-items:center;gap:7px}.legend-dot{display:inline-block;width:8px;height:8px;border-radius:2px}.legend-dot.factory{background:#5488ee}.legend-dot.diy{background:#d7a06b}.message-row{font-size:11px;color:#8496b0;margin-top:15px;min-height:30px;line-height:1.45}.side-panel{display:flex;flex-direction:column;gap:18px}.status-panel,.calibration-panel,.service-panel{padding:23px}.status-panel .panel-top{margin-bottom:18px}.live-icon{color:#6bb8d7}.source-card{border:1px solid #2b3b52;background:#111e2d;border-radius:10px;padding:16px;margin-bottom:12px}.hue-card{background:#1c1c2a;border-color:#433447}.source-head{display:flex;justify-content:space-between;align-items:center;gap:6px;margin-bottom:10px}.source-head>div{display:flex;gap:8px;align-items:center}.source-head strong{font-size:13px}.source-dot{width:8px;height:8px;border-radius:50%;display:inline-block;background:#66717d}.source-dot.live{background:#4bd8a4;box-shadow:0 0 9px #37d7a96b}.source-status{font-size:10px;color:#c1d2e6;border:1px solid #3a4a5a;border-radius:22px;padding:5px 8px}.source-type{font-size:11px;color:#93a0b5;display:flex;justify-content:space-between;gap:6px}.source-type span{font-size:10px;color:#627990;white-space:nowrap}.source-stats{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:18px 0 15px}.source-stats>div{display:flex;flex-direction:column;gap:6px}.source-stats span{font-size:10px;color:#8ea2ba}.source-stats strong{font-size:20px;font-weight:660;font-variant-numeric:tabular-nums}.side-count{font-size:11px;border-top:1px solid #2e3a4f;padding-top:11px;color:#a2b6ca}.side-count>span:first-child{font-size:15px;font-weight:800;color:#deedf9}.faint{color:#778ca5;margin-left:6px}.foot-info{font-size:10px;display:flex;gap:8px;color:#8492aa;line-height:1.6}.foot-info strong{color:#a9bfd3}.dot-neutral{height:8px;width:8px;border-radius:50%;flex-shrink:0;background:#638ab2;margin-top:4px}.calibration-panel .panel-top,.service-panel .panel-top{margin-bottom:18px}.calibration-panel label{display:flex;justify-content:space-between;font-size:11px;color:#aab9ce;font-weight:600;margin:14px 0 9px}.calibration-panel label span{color:#d9effb;font-weight:800}.calibration-panel select{width:100%;border:1px solid #35425a;background:#151d2c;padding:10px;border-radius:7px;color:#dce7f3;font-size:12px;outline:none}.calibration-panel input[type=range]{width:100%;accent-color:#65cdd0;cursor:pointer}.range-ends{display:flex;justify-content:space-between;font-size:10px;color:#7186a1}.power-readout{display:flex;justify-content:space-between;gap:10px;margin-top:22px}.power-readout>div{display:flex;flex-direction:column;gap:5px}.power-readout span{font-size:10px;color:#8d9eb6}.power-readout strong{font-size:17px;font-variant-numeric:tabular-nums}.meter{height:6px;background:#26344c;overflow:hidden;border-radius:15px;margin-top:14px}.meter>div{height:100%;width:0%;transition:width .13s;background:linear-gradient(90deg,#4dbda7,#70d8cc)}.disclaimer{font-size:10px;color:#8190a9;line-height:1.65;margin:13px 0 0}.lower{display:grid;grid-template-columns:minmax(0,1.8fr) minmax(295px,1fr);gap:18px;margin-top:18px}.diagnostics-panel{padding:22px 25px}.lower .panel-top h2{font-size:17px}.ghost-btn{padding:8px 11px;border:1px solid #31425c;border-radius:7px;background:#172238;color:#accde1;font-size:10px;font-weight:700}.ghost-btn:hover{background:#233454}.diagnostic-grid{display:grid;grid-template-columns:1fr 1fr;gap:16px}.diagnostic-grid>div{display:flex;flex-direction:column;gap:6px}.diagnostic-grid span{font-size:10px;color:#8296b0}.diagnostic-grid strong{font-size:12px;color:#d7e5f4;word-break:break-word}footer{display:flex;justify-content:space-between;gap:12px;font-size:9px;font-weight:650;letter-spacing:.06em;color:#667b98;margin:24px 0 0;flex-wrap:wrap}button:focus-visible,select:focus-visible,input:focus-visible{outline:2px solid #53c7df;outline-offset:2px}@media(max-width:1050px){.workspace,.lower{grid-template-columns:1fr}.side-panel{display:grid;grid-template-columns:1fr}.tv-area{min-height:330px}}@media(max-width:680px){.shell{padding:16px 11px 18px}.topbar{align-items:flex-start;gap:10px}.brand h1{font-size:14px}.brand p{font-size:9px}.brand-mark{transform:rotate(45deg) scale(.75);margin-left:-3px;margin-right:-5px}.pill{font-size:9px;padding:7px 8px}.top-actions{flex-direction:column;align-items:flex-end}.main-panel,.status-panel,.calibration-panel,.service-panel,.diagnostics-panel{padding:17px}.side-panel{grid-template-columns:1fr}.panel-top h2{font-size:18px}.subtle-label{display:none}.mode-switch{width:100%}.mode-switch button{flex:1}.tv-area{padding:28px 13px 17px;min-height:250px}.tv-centerline{min-height:140px}.tv-body{border-width:1px}.screen-kicker{font-size:8px}.screen-main{font-size:13px}.screen-foot{font-size:8px}.screen-icon{font-size:28px}.edge-legend{gap:12px;font-size:10px}.lower .panel-top{align-items:flex-start}.ghost-btn{max-width:124px;font-size:9px}footer{flex-direction:column;text-align:center;line-height:1.6}}
@media(min-width:1051px){.main-panel{display:flex;flex-direction:column}.tv-area{flex:1;min-height:455px}.message-row{margin-top:17px}}
.lamps-open-btn{display:inline-flex;align-items:center;gap:9px;border:1px solid #397287;background:linear-gradient(120deg,#133247,#10263a);color:#b7f1e6;font-weight:700;font-size:11px;cursor:pointer;padding:9px 12px;border-radius:10px;white-space:nowrap}
.lamps-open-btn:hover,.lamps-open-btn[aria-expanded="true"]{border-color:#6cd2c4;background:#173a4c}
.lamps-btn-icon{font-size:15px;color:#61d4bd}.lamps-btn-count{border:1px solid #2e5b69;background:#0a202f;border-radius:15px;padding:3px 6px;font-variant-numeric:tabular-nums;color:#eef9fb}
.lamps-backdrop{position:fixed;inset:0;z-index:120;background:rgba(2,8,17,.67);border:0;cursor:pointer}
.lamps-drawer{position:fixed;right:0;top:0;bottom:0;z-index:121;width:min(465px,100%);padding:25px 23px;overflow-y:auto;overscroll-behavior:contain;color:#dce8f6;background:#101a2b;border-left:1px solid #39516b;box-shadow:-15px 0 60px #0008}
.lamps-drawer-heading{display:flex;align-items:flex-start;justify-content:space-between;gap:10px}.lamps-drawer-heading h2{margin:8px 0;font-size:22px;letter-spacing:-.025em}.lamps-drawer-heading p{margin:0;font-size:11px;color:#a7bdd1}.lamps-close-btn{flex-shrink:0;border:1px solid #38516a;background:#182a40;color:#dce8f6;border-radius:8px;font-size:18px;padding:7px 11px;cursor:pointer}
.drawer-source{display:flex;align-items:center;gap:8px;border:1px solid #30425a;border-radius:9px;background:#111e31;padding:11px 12px;margin-top:25px;color:#c5d7e8;font-size:11px}.drawer-source>span:last-child{margin-left:auto;font-variant-numeric:tabular-nums;color:#a1b5ca}
.drawer-explanation{font-size:11px;line-height:1.7;color:#a5b6c9;margin:16px 0 20px}.drawer-explanation strong{color:#d0f3eb}
.drawer-lamps{display:grid;grid-template-columns:1fr;gap:13px}.drawer-lamp{border:1px solid #344763;border-radius:13px;background:#162438;padding:15px}.drawer-lamp-top{display:flex;gap:12px;align-items:center}.drawer-lamp-swatch{height:69px;width:69px;flex:none;border-radius:11px;border:1px solid #ffffff20;box-shadow:0 0 30px #ffffff0c;transition:background-color .07s linear}.drawer-lamp-heading{display:flex;flex-direction:column;gap:8px;min-width:0}.drawer-lamp-name{font-size:15px}.drawer-lamp-id{font-size:11px;color:#8fa9c2}.drawer-lamp-flag{margin-left:auto;align-self:start;white-space:nowrap;color:#b8c1d1;border:1px solid #405069;border-radius:6px;padding:4px 6px;font-size:9px;font-weight:800}.drawer-lamp-flag.lit{color:#80ebc5;border-color:#3b8f83;background:#153b3e}.lamp-inactive .drawer-lamp-swatch{box-shadow:none}
.drawer-lamp-metrics{display:grid;grid-template-columns:1fr;gap:8px;margin:15px 0 0}.drawer-metric{display:flex;justify-content:space-between;align-items:baseline;gap:12px;border-top:1px solid #2a3d54;padding-top:8px}.drawer-metric dt{font-size:10px;color:#97aac2}.drawer-metric dd{margin:0;text-align:right;font-size:11px;font-weight:650;color:#ebf3ff;overflow-wrap:anywhere;font-family:ui-monospace,SFMono-Regular,Consolas,monospace;font-variant-numeric:tabular-nums}.drawer-foot{margin-top:23px;padding:13px;border:1px solid #324256;border-radius:8px;color:#91a8c0;font-size:11px;line-height:1.6}
body.lamp-panel-open{overflow:hidden}
[hidden]{display:none!important}
button:disabled{opacity:.45;cursor:not-allowed}
.view-controls{margin:0 0 20px;padding:12px 14px;border:1px solid #2c3d55;border-radius:10px;background:#101c2c}
.view-controls label{display:flex;justify-content:space-between;gap:10px;color:#bfd1e4;font-size:11px;font-weight:650}
.view-controls output{color:#8fe1dc;font-variant-numeric:tabular-nums}
.view-gain-row{display:flex;align-items:center;gap:14px;margin-top:8px}
.view-gain-row input{flex:1;min-width:0;accent-color:#65cdd0;cursor:pointer}
.view-gain-row button{border:1px solid #38516a;border-radius:6px;padding:5px 10px;background:#172b40;color:#c7e6ef;font-size:11px}
.view-controls p{margin:8px 0 0;color:#91a8c0;font-size:10px;line-height:1.6}
.strip-summary{display:flex;flex-direction:column;align-items:center;gap:6px;margin-top:12px;text-align:center;font-size:10px;color:#9db6ca;line-height:1.5}.strip-summary strong{font-size:11px;color:#d1e8ed}.credentials-panel{padding:20px}.credentials-panel summary{font-size:14px;font-weight:700;cursor:pointer}.credentials-panel p{font-size:11px;color:#9eafc5;line-height:1.65}.credentials-panel label{display:block;margin:12px 0 6px;font-size:11px}.credentials-panel input{width:100%;padding:10px;border:1px solid #35425a;border-radius:7px;background:#111c2b;color:#e4eef8;font:inherit}.credentials-panel button{margin-top:15px;padding:10px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700}.credentials-panel input:focus-visible{outline:2px solid #53c7df;outline-offset:2px}
.mode-switch{flex-wrap:wrap}.mode-switch button{flex:1}
.source-stats .freshness-text{font-size:12px;line-height:24px}
.freshness-chart{margin:10px 0 15px}.freshness-chart svg{display:block;width:100%;height:66px;overflow:hidden;background:#091522;border-radius:6px}
.chart-grid{stroke:#263b51;stroke-width:1;fill:none}.chart-line{stroke:#72e3d2;stroke-width:2;fill:none;stroke-linejoin:round;stroke-linecap:round}
.hue-card .chart-line{stroke:#d5a5ff}.chart-caption{display:flex;justify-content:space-between;gap:6px;font-size:9px;color:#a4b9d0;margin-top:6px}
.service-panel button{margin:14px 10px 0 0;padding:10px 16px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700}
.service-panel button:hover:not(:disabled){background:#1f4a5d}
@media(max-width:680px){.mode-switch{display:grid;grid-template-columns:1fr 1fr}}
/* v0.19 (NEXT 1): input status in the top bar; details on hover / tap / focus */
.inputs-status{position:relative}
.inputs-chip{display:inline-flex;align-items:center;gap:10px;border:1px solid #2f4058;background:#121c2c;color:#b9c9dc;font-size:11px;font-weight:700;padding:9px 12px;border-radius:10px;white-space:nowrap;letter-spacing:.02em}
.inputs-chip:hover,.inputs-status.open .inputs-chip{border-color:#4a6788;background:#162337}
.inputs-chip .chip-src{display:inline-flex;align-items:center;gap:6px}
.inputs-chip b{font-weight:650;color:#e3eefa;font-variant-numeric:tabular-nums;min-width:4.2em;text-align:left}
.source-dot.warn{background:#ff8a72;box-shadow:0 0 8px #ff8a7299}
.inputs-chip .chip-sep{width:1px;height:14px;background:#33445c}
.chip-conn{display:inline-flex;align-items:center;gap:6px;color:#c9d7e8}
.chip-conn.offline .signal{background:#e0777c;box-shadow:0 0 6px #b94854}
.chip-conn.online .signal{background:#51e0ae;box-shadow:0 0 7px #4dcbaa}
.inputs-chip.offline{border-color:#7a3a44;background:#24151b;color:#f0b9bd}
.inputs-chip.offline .chip-conn{color:#f4c3c6}
@media(max-width:460px){.chip-conn #connectionText{display:none}}
.inputs-pop{position:absolute;right:0;top:calc(100% + 10px);width:min(370px,calc(100vw - 32px));max-height:calc(100vh - 110px);overflow:auto;z-index:60;display:none;background:#111a2a;box-shadow:0 18px 50px #000c}
.inputs-pop::before{content:'';position:absolute;left:0;right:0;top:-12px;height:12px}
.inputs-status.open .inputs-pop{display:block}.pop-close{border:1px solid #31425c;background:#172238;color:#accde1;border-radius:7px;width:30px;height:30px;font-size:14px;line-height:1}.pop-close:hover{background:#233454}
.inputs-status.closed .inputs-pop{display:none!important}
@media(max-width:1050px){.inputs-pop{position:fixed;left:16px;right:16px;top:auto;width:auto;max-width:520px;margin:8px auto 0}}
@media(max-width:680px){.inputs-chip{font-size:10px;padding:7px 9px;gap:7px}.inputs-chip b{min-width:3.4em}}
/* v10: tabs */
.tabs{display:flex;gap:6px;margin:-8px 0 18px;border-bottom:1px solid #22304a;flex-wrap:wrap}
.tabs button{position:relative;background:transparent;border:0;border-bottom:2px solid transparent;color:#8ea0b8;padding:10px 14px;font-size:12px;font-weight:750;letter-spacing:.04em;margin-bottom:-1px}
.tabs button[aria-selected="true"]{color:#e3f2ff;border-bottom-color:#5fd0cf}
.tabs button:hover{color:#cfe3f5}
.tab-badge{margin-left:7px;font-size:9px;padding:2px 6px;border-radius:10px;background:#1d4a3a;color:#8ff0c6;border:1px solid #2f7a5c;animation:pulse 1.2s ease-in-out infinite}
@keyframes pulse{50%{opacity:.45}}
/* v10: TV first, wall glow + estimated picture */
.tv-area{position:relative;overflow:hidden;isolation:isolate}
@media(min-width:1051px){.workspace{align-items:start}.tv-area{flex:none;min-height:430px}}
.strip-summary,.edge-legend{position:relative;z-index:1;text-shadow:0 1px 3px #000d}
.wall-glow{position:absolute;inset:0;width:100%;height:100%;z-index:0;filter:blur(20px) saturate(1.2);opacity:.8;pointer-events:none;transition:opacity .4s}
.tv-assembly{position:relative;z-index:1}
.screen-fill{position:absolute;inset:0;width:100%;height:100%;z-index:0;filter:blur(18px);transform:scale(1.15);opacity:.85;border-radius:8px;pointer-events:none}
.screen-fill.off{display:none}
.tv-body .tv-screen-content,.tv-body .screen-bottom{position:relative;z-index:1}
.tv-body .screen-bottom{position:absolute;background:#0a0f1a66}
.tv-body.filled .screen-glow{display:none}
.tv-body.filled .tv-screen-content>*{text-shadow:0 1px 3px #000c,0 0 14px #0009;color:#f1f6fc}
.tv-body.filled .screen-icon{opacity:.55}
.edge .pixel{border-radius:4px}
.edge-top .pixel,.edge-bottom .pixel{border-radius:5px}
/* v10: two clear view choices instead of four modes */
.view-options{display:grid;grid-template-columns:1fr 1fr;gap:12px 18px;margin:18px 0 14px}
.seg-group{display:flex;flex-direction:column;gap:7px;min-width:0}
.seg-label{font-size:10px;font-weight:800;letter-spacing:.12em;color:#7fb4c9;text-transform:uppercase}
.view-options .mode-switch{margin:0;width:100%}
.view-options .mode-switch button{flex:1;white-space:nowrap}
.view-options .mode-switch small{display:inline-block;margin-left:5px;font-size:10px;font-weight:650;color:#7f93ad}
.view-options .mode-switch button.selected small{color:#9fe3dc}
.toggles{grid-column:1/-1;display:flex;flex-wrap:wrap;gap:8px 18px;align-items:center;font-size:11px;color:#bfd1e4}
.toggles label{display:inline-flex;gap:7px;align-items:center;cursor:pointer}
.toggles input{accent-color:#65cdd0}
.toggle-note{font-size:10px;color:#7f93ad}
@media(max-width:680px){.view-options{grid-template-columns:1fr}.view-options .mode-switch{display:flex}}
/* v10: bridge tab */
.bridge-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:18px;align-items:start}
.bridge-grid .credentials-panel[open] summary{margin-bottom:6px}
.future-panel{padding:23px}
.future-panel .soon{font-size:9px;font-weight:800;letter-spacing:.12em;color:#f2c27a;border:1px solid #6d5631;border-radius:10px;padding:3px 7px;margin-left:8px;vertical-align:middle}
.future-row{display:flex;justify-content:space-between;align-items:center;gap:12px;border-top:1px solid #26354c;padding:12px 0;font-size:12px;color:#c9d8e8}
.future-row span{color:#8296b0;font-size:11px}
.future-row select,.future-row button{border:1px solid #35425a;background:#151d2c;color:#7d8da3;padding:7px 10px;border-radius:7px;font-size:11px}
.pair-hero{display:flex;align-items:center;gap:14px;margin:6px 0 4px;flex-wrap:wrap}
.pair-hero #btnPair{margin:0;padding:12px 20px;font-size:13px}
.pair-hero.open #btnPair{background:#1d4a3a;border-color:#4fc597;color:#d9fff0}
/* v0.15: "the TV wants to pair" banner (both tabs) */
.pair-banner{display:flex;align-items:center;justify-content:space-between;gap:14px;flex-wrap:wrap;margin:0 0 16px;padding:14px 18px;border-radius:12px;border:1px solid #8a6a2e;background:#2a2112;color:#f6e3bd;font-size:13px;line-height:1.5}
.pair-banner strong{display:block;font-size:15px;color:#ffd58a}
.pair-banner.open{border-color:#4fc597;background:#12291f;color:#cdeedd}.pair-banner.open strong{color:#8ff0c6}
.pair-banner.done{border-color:#3d8f6a;background:#10241b;color:#cdeedd}.pair-banner.done strong{color:#8ff0c6}
.pair-banner button{padding:12px 22px;border:1px solid #e0a94f;border-radius:8px;background:#5a4214;color:#fff1d6;font-weight:800;font-size:14px;cursor:pointer}
.pair-banner button:focus-visible{outline:2px solid #ffd58a;outline-offset:2px}
.pair-banner.old-session{animation:none;border-color:#b5604f;background:#2a1513;color:#f3d6cf}.pair-banner.old-session strong{color:#ff9f8a}.pair-banner.old-session span{display:block}
@keyframes pairGlow{0%,100%{box-shadow:0 0 0 0 #e0a94f00}50%{box-shadow:0 0 0 4px #e0a94f55}}
.pair-banner:not(.open):not(.done){animation:pairGlow 1.6s ease-in-out infinite}
@media(prefers-reduced-motion:reduce){.pair-banner{animation:none!important}}
/* v0.19: who asks */
.discovery-panel{padding:20px;grid-column:1/-1}
.disc-switch{display:flex;gap:10px;align-items:flex-start;margin:10px 0 12px;padding:11px 13px;border:1px solid #2b3b52;border-radius:9px;background:#111e2d;font-size:11px;color:#9eafc5;line-height:1.55;cursor:pointer}
.disc-switch strong{color:#dceeff;font-size:12px}.disc-switch input{margin-top:3px;accent-color:#65cdd0}
.disc-list{display:flex;flex-direction:column;gap:8px}
.disc-item{border:1px solid #26354c;border-radius:9px;padding:10px 12px;background:#0f1828;font-size:11px;color:#a9bbd0;line-height:1.5;min-width:0}
.disc-item .disc-head{display:flex;flex-wrap:wrap;gap:6px 12px;align-items:baseline;color:#dce7f3}
.disc-item .disc-head strong{font-size:12px}.disc-item .disc-tag{font-size:9px;font-weight:800;letter-spacing:.08em;border-radius:9px;padding:2px 7px;border:1px solid #3a4a5a;color:#9fb3c9}
.disc-item .disc-tag.hue{border-color:#3d8f6a;color:#8ff0c6}.disc-item .disc-tag.ignored{border-color:#8a6a2e;color:#ffd58a}
.disc-item .disc-ua,.disc-item .disc-st{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:10.5px;color:#c9d8e8;overflow-wrap:anywhere;word-break:break-word}
.disc-item .disc-guess{color:#9fe3dc}
/* v0.28: service controls, stopped banner, login */
.service-buttons{display:flex;gap:8px;flex-wrap:wrap;margin-top:14px}
.service-buttons button{flex:1 1 90px;padding:10px 12px;border:1px solid #35425a;border-radius:8px;background:#151d2c;color:#d6e3f1;font-weight:700}
.service-buttons #btnStart{border-color:#3d8f6a;background:#10241b;color:#bff3da}
.service-buttons #btnStop{border-color:#8a6a2e;background:#2a2112;color:#f6e3bd}
.service-buttons #btnExit{border-color:#7a3f3a;background:#24130f;color:#f3d6cf}
.pair-banner.service-stopped{animation:none;border-color:#7a3f3a;background:#24130f;color:#f3d6cf}.pair-banner.service-stopped strong{color:#ffb1a1}.pair-banner.service-stopped span{display:block}
.pair-banner.service-stopped button{border-color:#3d8f6a;background:#10241b;color:#bff3da}
.login-pill{cursor:pointer;background:#172238;border-color:#31425c;color:#cfe0f3}
.login-pill.in{border-color:#3d8f6a;color:#8ff0c6}
.login-dialog{border:1px solid #31425c;border-radius:14px;background:#111725;color:#e8edf6;padding:20px;width:min(380px,calc(100vw - 32px))}
.login-dialog::backdrop{background:#02050bcc}
.login-dialog h2{margin:0 0 6px;font-size:17px}
.login-dialog label{display:block;margin:12px 0 5px;font-size:11px;font-weight:700;color:#d6e3f1}
.login-dialog input{width:100%;padding:9px 10px;border:1px solid #35425a;border-radius:7px;background:#111c2b;color:#e4eef8;font-size:13px}
.login-dialog .row{display:flex;gap:8px;justify-content:flex-end;margin-top:16px}
.login-dialog button{padding:9px 14px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700}
.login-dialog button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
.login-dialog p{font-size:11px;color:#9eafc5;line-height:1.6;margin:6px 0 0}
.login-dialog .err{color:#ff9f8a;min-height:1.6em}
.access-panel .warn{color:#ffd58a}
/* v0.17: identity card */
.identity-panel{padding:20px;grid-column:1/-1}
@media(min-width:1000px){.identity-panel{grid-column:2/-1}.id-form{display:grid;grid-template-columns:1fr 1fr;column-gap:18px}.id-form .id-new{grid-column:1/-1}}
.discovery-panel .id-intro,.discovery-panel .id-note,.identity-panel .id-intro,.identity-panel .id-note{font-size:11px;color:#9eafc5;line-height:1.6}
.id-list{display:flex;flex-direction:column;gap:8px;margin:12px 0}
.id-item{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:10px 12px;border:1px solid #2a3a52;border-radius:9px;background:#0f1724;font-size:12px;color:#d6e3f1;cursor:pointer;text-align:left;width:100%;font:inherit}
.id-item[aria-checked="true"]{border-color:#4fb3c2;background:#122634}
.id-item:focus-visible{outline:2px solid #53c7df;outline-offset:2px}
.id-item .id-main{display:flex;flex-direction:column;gap:3px;min-width:0}
.id-item .id-main b{font-size:13px;overflow-wrap:anywhere}
.id-item .id-main span{font-size:10px;color:#8ea2bb;font-family:ui-monospace,monospace;overflow-wrap:anywhere}
.id-badges{display:flex;gap:5px;flex-wrap:wrap;justify-content:flex-end}
.id-badge{font-size:9px;font-weight:800;letter-spacing:.08em;padding:3px 7px;border-radius:9px;border:1px solid #35425a;color:#9fb4cb;white-space:nowrap}
.id-badge.run{border-color:#2f7a5c;color:#8ff0c6}.id-badge.sel{border-color:#4fb3c2;color:#9fe3ec}
.id-form label{display:block;margin:12px 0 5px;font-size:11px;font-weight:700;color:#d6e3f1}
.id-row{display:flex;gap:8px;flex-wrap:wrap}
.id-row input,.id-row select{flex:1 1 140px;min-width:0;padding:9px 10px;border:1px solid #35425a;border-radius:7px;background:#111c2b;color:#e4eef8;font:inherit;font-size:13px}
.id-row button{padding:9px 13px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700;cursor:pointer}
.id-row button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
.id-row button:disabled,.id-row input:disabled,.id-row select:disabled{opacity:.55;cursor:default}
.id-new{margin-top:14px}
.identity-panel .hint{display:block;margin-top:4px;font-size:10px;color:#8ea2bb}
.id-restart{margin-top:12px;padding:11px 13px;border:1px solid #8a6a2e;border-radius:8px;background:#2a2112;color:#f6e3bd;font-size:11px;line-height:1.6}
.id-restart code{color:#ffd58a}
/* v0.15 (Phase 3): strip & current settings form */
.strip-settings{padding:20px}
.strip-settings{grid-column:1/-1}
@media(min-width:1000px){.strip-settings{grid-column:2/-1}}
.strip-settings .fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:4px 18px}
.strip-settings label{display:block;margin:12px 0 5px;font-size:11px;font-weight:700;color:#d6e3f1}
.strip-settings input{width:100%;padding:9px 10px;border:1px solid #35425a;border-radius:7px;background:#111c2b;color:#e4eef8;font:inherit;font-size:13px}
.strip-settings select{width:100%;max-width:100%;min-width:0;padding:9px 10px;border:1px solid #35425a;border-radius:7px;background:#111c2b;color:#e4eef8;font:inherit;font-size:13px;text-overflow:ellipsis}
.strip-settings select:disabled{color:#8a9ab2}
.strip-settings input:focus-visible{outline:2px solid #53c7df;outline-offset:2px}
.strip-settings input:disabled{color:#8a9ab2}
.strip-settings .hint{display:block;margin-top:4px;font-size:10px;color:#8ea2bb;line-height:1.5}
.strip-settings .wb-row{display:grid;grid-template-columns:repeat(3,1fr);gap:6px}
.strip-settings .hint.warn{color:#f2c27a}
.strip-settings .muted{opacity:.45}
.strip-settings .summary{margin:16px 0 0;padding:11px 13px;border:1px solid #2a3a52;border-radius:8px;background:#0f1724;font-size:11px;color:#bcd0e3;line-height:1.6}
.strip-settings .summary b{color:#e6f2fb}
.strip-settings .actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:15px}
.strip-settings .actions button{padding:10px 14px;border:1px solid #407d8a;border-radius:7px;background:#173747;color:#d6eff0;font-weight:700;cursor:pointer}
.strip-settings .actions button.secondary{border-color:#35425a;background:#151d2c;color:#b8c7da}
.strip-settings .actions button:disabled{opacity:.5;cursor:default}
.strip-settings p{font-size:11px;color:#9eafc5;line-height:1.65}
.strip-log-panel{padding:23px}
/* v0.44 (user: «την καταπόνηση όπως τα mA, με βελάκι να κρύβεις τα πολλά»): in the lower row next to
   «Τεχνικά δεδομένα»; the bar and one line of totals always shown, the rest folded */
.lower{align-items:start}
.strip-log-panel details.log-more{margin-top:12px}
.strip-log-panel details.log-more>summary{font-size:11px;color:#9fb2c9;font-weight:650;list-style:none}
.strip-log-panel details.log-more>summary::-webkit-details-marker{display:none}
.strip-log-panel details.log-more>summary::before{content:'▸';margin-right:6px;color:#6db0c7}
.strip-log-panel details.log-more[open]>summary::before{content:'▾'}
.strip-log-panel .panel-top{margin-bottom:16px;flex-wrap:wrap}
.log-switch{margin-bottom:0;width:auto}.log-switch button{padding:6px 9px;font-size:10px;white-space:nowrap;flex:none}
.wear-bar{display:flex;gap:2px;height:20px}
.wear-bar i{flex:1;min-width:0;border-radius:2px;background:#26344c;position:relative}
.wear-bar i.top::after{content:'';position:absolute;left:50%;bottom:-7px;width:4px;height:4px;margin-left:-2px;border-radius:50%;background:#f2ae65}
.wear-legend{display:flex;justify-content:space-between;align-items:center;gap:8px;font-size:10px;color:#7f93ad;margin:10px 0 14px}
.wear-scale{display:inline-flex;align-items:center;gap:6px}.wear-scale i{display:inline-block;width:60px;height:6px;border-radius:3px;background:linear-gradient(90deg,#24456e,#3f9fb0,#e7b05c,#e8604f)}
.log-totals{font-size:11px;color:#a9bbd0;line-height:1.6}.log-totals b{color:#e3eefa;font-weight:650}
.log-top{display:flex;flex-wrap:wrap;align-items:center;gap:6px;margin:10px 0 4px;font-size:10px;color:#8fa4bd}
.log-top span{font-size:10px;border:1px solid #3a4a5a;border-radius:6px;padding:4px 6px;color:#cfdbe9;background:#132033}
.log-top span b{color:#f2c27e}
.strip-log-panel details{margin-top:14px;border-top:1px solid #263247;padding-top:10px}
.strip-log-panel summary{cursor:pointer;font-size:12px;font-weight:700;color:#cfe0f0}
.strip-log-panel summary span{font-size:10px;color:#8fa4bd;border:1px solid #3a4a5a;border-radius:10px;padding:1px 6px;margin-left:4px}
.log-help{font-size:10px;color:#7f93ad;line-height:1.55;margin:8px 0}
#logEpList{list-style:none;margin:0;padding:0;display:flex;flex-direction:column;gap:7px;max-height:330px;overflow:auto}
#logEpList li{font-size:11px;line-height:1.5;color:#b8c8da;border:1px solid #2b3b52;background:#111e2d;border-radius:8px;padding:8px 10px}
#logEpList li b{color:#e6f0fa;font-weight:650}#logEpList li .now{color:#f2ae65;font-weight:800;margin-left:6px}
#logEpList li .ep-sub{display:block;color:#8ea2ba;font-size:10px}
.log-days table{width:100%;border-collapse:collapse;font-size:10px;margin-top:8px;font-variant-numeric:tabular-nums}
.log-days th{text-align:left;color:#7f93ad;font-weight:600;padding:3px 4px;border-bottom:1px solid #263247}
.log-days td{padding:3px 4px;color:#c4d2e2;border-bottom:1px solid #1b2638}
.log-actions{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:14px}
.log-note{font-size:10px;color:#8fa4bd}
</style>
</head>
<body>
<main class="shell">
<header class="topbar">
  <div class="brand"><div class="brand-mark"><span></span><span></span><span></span><span></span></div>
    <div><h1>AMBILIGHT <em>MONITOR</em></h1><p>4th-Side Lab <span class="microdot">·</span> ambi-bridge-cpp</p></div>
  </div>
  <div class="top-actions">
    <div class="inputs-status" id="inputsStatus">
      <button type="button" id="inputsToggle" class="inputs-chip offline" aria-expanded="false" aria-controls="inputsPopover" title="Σύνδεση και κατάσταση εισόδων: πάτα για λεπτομέρειες (κλείνουν με κλικ έξω, ✕ ή Esc)">
        <span id="connection" class="chip-conn offline"><span class="signal"></span><span id="connectionText">Σύνδεση…</span></span>
        <span class="chip-sep" aria-hidden="true"></span>
        <span class="chip-src"><span class="source-dot" id="jsChipDot"></span>JointSpace <b id="jsChipRate">—</b></span>
        <span class="chip-sep" aria-hidden="true"></span>
        <span class="chip-src"><span class="source-dot" id="hueChipDot"></span>Hue <b id="hueChipRate">—</b></span>
      </button>
    <div class="panel status-panel inputs-pop" id="inputsPopover" role="region" aria-label="Κατάσταση εισόδων: λεπτομέρειες">
      <div class="panel-top"><div><div class="eyebrow">SOURCES</div><h2>Σύνδεση &amp; είσοδοι</h2></div><button type="button" class="pop-close" id="inputsClose" aria-label="Κλείσιμο">✕</button></div>
      <div class="source-card page-card">
        <div class="source-head"><div><span class="source-dot" id="pageDot"></span><strong>Σελίδα ↔ bridge</strong></div><span id="pageStatus" class="source-status">Σύνδεση…</span></div>
        <div class="source-type">Αυτή η σελίδα <span>SSE /events</span></div>
        <div class="source-stats"><div><span>Τελευταία ενημέρωση</span><strong id="pageAge">—</strong></div><div><span>Ενημερώσεις / s</span><strong id="pageRate">—</strong></div></div>
      </div>
      <div class="source-card">
        <div class="source-head"><div><span class="source-dot" id="jsDot"></span><strong>JointSpace</strong></div><span id="jsStatus" class="source-status">Αναμονή</span></div>
        <div class="source-type">Εργοστασιακές πλευρές <span>HTTPS :1926</span></div>
        <div class="source-stats"><div><span>Samples / s</span><strong id="jsFps">—</strong></div><div><span>Freshness</span><strong class="freshness-text" id="jsFreshness">—</strong></div></div>
        <div class="freshness-chart"><svg id="jsChart" viewBox="0 0 300 60" role="img" aria-label="Ηλικία δείγματος, όχι RTT"><path class="chart-grid" d="M0 6H300 M0 30H300 M0 54H300"/><path id="jsChartLine" class="chart-line"/></svg><div class="chart-caption"><span>−30 s</span><span id="jsChartScale">Ηλικία δείγματος · όχι RTT</span><span>τώρα</span></div></div>
        <div class="side-count"><span id="jsCount">0</span> ζώνες <span class="faint" id="jsBreakdown">3 / 8 / 3 αναμενόμενες</span></div>
      </div>
      <div class="source-card hue-card">
        <div class="source-head"><div><span class="source-dot" id="hueDot"></span><strong>Hue Entertainment</strong></div><span id="hueStatus" class="source-status">Αναμονή</span></div>
        <div class="source-type">Κάτω πλευρά <span>DTLS UDP</span></div>
        <div class="source-stats"><div><span>Frames / s</span><strong id="hueFps">—</strong></div><div><span>Freshness</span><strong class="freshness-text" id="hueFreshness">—</strong></div></div>
        <div class="freshness-chart"><svg id="hueChart" viewBox="0 0 300 60" role="img" aria-label="Ηλικία δείγματος, όχι RTT"><path class="chart-grid" d="M0 6H300 M0 30H300 M0 54H300"/><path id="hueChartLine" class="chart-line"/></svg><div class="chart-caption"><span>−30 s</span><span id="hueChartScale">Ηλικία δείγματος · όχι RTT</span><span>τώρα</span></div></div>
        <div class="side-count"><span id="hueCount">0</span> / <span id="hueTotal">3</span> λάμπες <span class="faint" id="hueVersion">HueStream v—</span></div>
      </div>
      <div class="foot-info"><span class="dot-neutral"></span><span>Τα stale / ανενεργά σήματα <strong>δεν</strong> εμφανίζονται ως live.</span></div>
    </div>
    </div>
    <button type="button" id="virtualLampsButton" class="lamps-open-btn" aria-controls="virtualLampsPanel" aria-expanded="false" aria-haspopup="dialog"><span class="lamps-btn-icon" aria-hidden="true">◉</span> Εικονικές Hue λάμπες <span id="lampButtonCount" class="lamps-btn-count">0/3</span></button>
    <button type="button" id="loginButton" class="pill login-pill" hidden>Σύνδεση</button>
    <span id="demoBadge" class="pill demo hidden">SYNTHETIC DEMO</span>
  </div>
</header>

<nav class="tabs" role="tablist" aria-label="Ενότητες">
  <button type="button" role="tab" id="tabLive" data-tab="live" aria-selected="true" aria-controls="paneLive">Live</button>
  <button type="button" role="tab" id="tabBridge" data-tab="bridge" aria-selected="false" aria-controls="paneBridge">Bridge &amp; ρυθμίσεις<span id="tabPairBadge" class="tab-badge" hidden>PAIR</span></button>
</nav>

<div id="pairBanner" class="pair-banner" role="status" aria-live="polite" hidden>
  <div><strong id="pairBannerTitle">Η TV ζητά σύζευξη</strong><span id="pairBannerText">Πάτα Pair.</span></div>
  <button id="btnPairBanner" type="button">Pair</button>
</div>

<div id="serviceBanner" class="pair-banner service-stopped" role="status" aria-live="polite" hidden>
  <div><strong>Η υπηρεσία είναι σταματημένη</strong><span>Το bridge δεν απαντά στην TV (Hue, JointSpace) και δεν στέλνει στην ταινία. Τρέχουν μόνο το terminal και αυτή η σελίδα. Start εδώ ή <b>s</b> στο terminal.</span></div>
  <button id="btnStartBanner" type="button">Start</button>
</div>

<div id="oldSessionBanner" class="pair-banner old-session" role="alert" hidden>
  <div><strong>Η TV στέλνει Hue από παλιά σύνδεση</strong><span id="oldSessionText">Το bridge ξεκίνησε ξανά ενώ η TV έστελνε, και η TV συνεχίζει με τα κλειδιά της προηγούμενης σύνδεσης, που δεν διαβάζονται. Από τη v0.37 το bridge ζητά από την TV να ξανασυνδεθεί μόνη της (αποτυγχάνει επίτηδες ο έλεγχος λάμπας της): περίμενε περίπου 15–30 s. Αν δεν ξαναρχίσει, στην TV κλείσε και ξανάνοιξε το <b>Ambilight + hue</b>. Δεν χρειάζεται νέα σύζευξη.</span></div>
</div>

<div id="tvLockBanner" class="pair-banner old-session" role="alert" hidden>
  <div><strong>Η TV κλείδωσε σε LightGroup μετά από αποτυχία DTLS — χρειάζεται restart της TV</strong><span id="tvLockText"></span></div>
</div>

<div id="paneLive" role="tabpanel" aria-labelledby="tabLive">
<section class="workspace">
  <div class="main-panel panel">
    <div class="panel-top"><div><div class="eyebrow">LIVE COLOR MAP</div><h2>Οι τέσσερις πλευρές</h2></div><div class="subtle-label">Εμπρόσθια όψη <span class="sep">/</span> Χωρική διάταξη</div></div>
    <div class="tv-area">
      <canvas id="wallGlow" class="wall-glow" width="160" height="100" aria-hidden="true"></canvas>
      <div class="tv-assembly">
        <div id="topEdge" class="edge edge-top" aria-label="Πάνω Ambilight"></div>
        <div class="tv-centerline">
          <div id="leftEdge" class="edge edge-vertical" aria-label="Αριστερό Ambilight"></div>
          <div class="tv-body" id="tvBody">
            <canvas id="screenFill" class="screen-fill off" width="32" height="18" aria-hidden="true"></canvas>
            <div class="screen-glow"></div>
            <div class="tv-screen-content"><span class="screen-kicker">PHILIPS OLED</span><div class="screen-icon">✦</div><div class="screen-main">4th-Side Ambilight</div><span class="screen-foot" id="screenFoot">LIVE AMBILIGHT CAPTURE</span></div>
            <div class="screen-bottom"><span>RGB / XY+Bri</span><span id="frameMark">NO SIGNAL</span></div>
          </div>
          <div id="rightEdge" class="edge edge-vertical" aria-label="Δεξί Ambilight"></div>
        </div>
        <div id="bottomEdge" class="edge edge-bottom" aria-label="Προσομοίωση της κάτω ταινίας LED"></div>
        <div class="strip-summary"><strong id="stripSummary">Ταινία LED</strong><span id="cornerStatus">Αναμονή Hue stream</span></div>
      </div>
      <div class="edge-legend"><span><i class="legend-dot factory"></i> OEM / JointSpace</span><span><i class="legend-dot diy"></i> DIY / υπολογισμένη ταινία LED</span></div>
    </div>
    <div class="view-options">
      <div class="seg-group"><span class="seg-label">Πλευρές TV · JointSpace</span>
        <div class="mode-switch" role="group" aria-label="Πώς δείχνονται οι πλευρές">
          <button type="button" data-sides="zones" class="selected" aria-pressed="true">Ζώνες<small id="zonesLabel">3/8/3</small></button>
          <button type="button" data-sides="leds" aria-pressed="false">Θέσεις LED<small>9/28/9</small></button>
        </div>
      </div>
      <div class="seg-group"><span class="seg-label">Κάτω πλευρά · Hue</span>
        <div class="mode-switch" role="group" aria-label="Πώς δείχνεται η κάτω πλευρά">
          <button type="button" data-bottom="lamps" class="selected" aria-pressed="true">Λάμπες<small id="lampsLabel">3</small></button>
          <button type="button" data-bottom="zones" aria-pressed="false">Ζώνες<small id="bottomZonesLabel">3</small></button>
          <button type="button" data-bottom="leds" aria-pressed="false" title="28 θέσεις LED, όσες η πάνω πλευρά">Θέσεις<small>28</small></button>
          <button type="button" data-bottom="strip" aria-pressed="false" title="Η ταινία ακριβώς όπως πάει στο WLED">Ταινία<small id="stripLabel">—</small></button>
        </div>
      </div>
      <div class="toggles">
        <label><input type="checkbox" id="optScreenFill" checked> Εικόνα στο κέντρο (εκτίμηση)</label>
        <label><input type="checkbox" id="optWallGlow" checked> Λάμψη στον τοίχο</label>
        <span class="toggle-note">Εκτίμηση από τα χρώματα Ambilight, όχι η πραγματική εικόνα της TV.</span>
      </div>
    </div>
    <div class="view-controls">
      <label for="displayGain">Φωτεινότητα προβολής <output id="displayGainValue" for="displayGain">1×</output></label>
      <div class="view-gain-row"><input id="displayGain" type="range" min="0.5" max="4" value="1" step="0.25"><button id="resetDisplayGain" type="button">1×</button></div>
      <p>Μόνο για την οθόνη · τα δεδομένα και η TV δεν αλλάζουν. Σχετικό προφίλ Cast · σταθερό λευκό JointSpace 23 · προσεγγιστική αντιστοίχιση Hue, όχι φυσική βαθμονόμηση.</p>
    </div>
    <div class="message-row" id="modeNote"></div>
  </div>

  <aside class="side-panel">
    <div class="panel calibration-panel">
      <div class="panel-top"><div><div class="eyebrow">OUTPUT</div><h2>Έξοδος ταινίας</h2></div><span class="source-dot" id="wledDot"></span></div>
      <div class="power-readout"><div><span>Εκτιμώμενο ρεύμα</span><strong id="powerEstimate">—</strong></div><div><span>Όριο (budget)</span><strong id="powerBudget">—</strong></div></div>
      <div class="meter"><div id="powerMeter"></div></div>
      <div class="diagnostic-grid" style="margin-top:18px">
        <div><span>LED</span><strong id="outLeds">—</strong></div>
        <div><span>mA / LED σε λευκό</span><strong id="outPerLed">—</strong></div>
        <div><span>Τρόπος ορίου</span><strong id="outMode">—</strong></div>
        <div><span>Κορυφές / ελάχιστο LED</span><strong id="outScales">—</strong></div>
        <div><span>peak_emphasis · κορυφές</span><strong id="outPeaks">—</strong></div>
        <div><span>rise_ms</span><strong id="outRise">—</strong></div>
        <div><span>Ζητούσε → στάλθηκε</span><strong id="outWanted">—</strong></div>
        <div><span>Κυλιόμενες κορυφές</span><strong id="outDrift">—</strong></div>
        <div><span>Σημεία (anchors)</span><strong id="outAnchors">—</strong></div>
        <div><span>Ίδιο καρέ με pe=0</span><strong id="outCompare">—</strong></div>
        <div><span>WLED (DDP)</span><strong id="wledTarget">—</strong></div>
        <div><span>Καρέ / s προς WLED</span><strong id="wledFps">—</strong></div>
        <div><span>Σφάλματα αποστολής</span><strong id="wledErrors">—</strong></div>
      </div>
      <p class="disclaimer">Ό,τι δείχνει η «Ταινία → WLED» είναι ακριβώς ό,τι υπολογίζει και στέλνει το bridge (γραμμικές τιμές LED, μετά το όριο ρεύματος και το rise_ms). Οι τελείες κάτω από την ταινία είναι οι κορυφές του ορίου. Το «Ίδιο καρέ με pe=0» είναι το ρεύμα με το παλιό όριο· όταν το καρέ χωράει στο όριο είναι ίδιο, γιατί τότε κανένα από τα δύο δεν χαμηλώνει τίποτα. Το ρεύμα είναι εκτίμηση από τις ρυθμίσεις, όχι μέτρηση. Η μετάβαση χρωμάτων και οι γωνίες ακολουθούν τις ρυθμίσεις της καρτέλας Bridge· βαθμονόμηση χρώματος δεν υπάρχει ακόμα. Το WLED μπορεί να χαμηλώσει κι άλλο με το δικό του ABL.</p>
    </div>

  </aside>
</section>

<section class="lower">
  <div class="panel diagnostics-panel">
    <div class="panel-top"><div><div class="eyebrow">DIAGNOSTICS</div><h2>Τεχνικά δεδομένα</h2></div><button type="button" class="ghost-btn" id="copyButton">Αντιγραφή diagnostics</button></div>
    <div class="diagnostic-grid"><div><span>Σχήμα API</span><strong id="schema">—</strong></div><div><span>JointSpace layers</span><strong id="layers">—</strong></div><div><span>HueStream colorspace</span><strong id="colorspace">—</strong></div><div><span>Malformed frames</span><strong id="badFrames">—</strong></div><div><span>Ενημέρωση browser</span><strong id="browserAge">—</strong></div><div><span>Προέλευση δεδομένων</span><strong id="origin">—</strong></div></div>
  </div>
  <div class="panel strip-log-panel" id="stripLogPanel">
    <div class="panel-top"><div><div class="eyebrow">LOG</div><h2>Καταπόνηση LED &amp; συμβιβασμοί</h2></div>
      <div class="mode-switch log-switch" role="group" aria-label="Ποια σύνολα"><button type="button" data-logscope="since_reset" class="selected" aria-pressed="true">Από το reset</button><button type="button" data-logscope="since_install" aria-pressed="false">Από την τοποθέτηση</button><button type="button" data-logscope="all_time" aria-pressed="false">Από πάντα</button></div></div>
    <div class="wear-bar" id="wearBar" aria-label="Καταπόνηση ανά LED (εκτιμώμενα mAh)"></div>
    <div class="wear-legend"><span>LED 1</span><span class="wear-scale"><i></i>λίγο → πολύ (mAh)</span><span id="wearLast">—</span></div>
    <div class="log-totals" id="logTotals">Αναμονή δεδομένων…</div>
    <details class="log-more" id="logMore">
      <summary>Λεπτομέρειες: πιο φορτωμένα LED, επεισόδια, μέρες, μηδενισμός</summary>
      <div class="log-top" id="logTop"></div>
      <details class="log-episodes" id="logEpisodes" open>
        <summary>Ιστορικό συμβιβασμών <span id="logEpCount">0</span></summary>
        <p class="log-help">Ένα επεισόδιο = συνεχόμενα καρέ που δεν χωρούσαν στο όριο (κενά κάτω από 1.5 s ενώνονται). Πόσο ζητούσε η εικόνα, πόσο στάλθηκε, ποιες κορυφές κρατήθηκαν και πόσο χαμήλωσαν τα υπόλοιπα, στο χειρότερο καρέ.</p>
        <ol id="logEpList"></ol>
      </details>
      <details class="log-days" id="logDays">
        <summary>Τελευταίες 30 μέρες</summary>
        <table><thead><tr><th>Μέρα</th><th>Ροή</th><th>Πάνω από όριο</th><th>Επεισόδια</th><th>mAh</th></tr></thead><tbody id="logDaysBody"></tbody></table>
      </details>
      <div class="actions log-actions"><button id="logReset" type="button" class="ghost-btn" disabled>Μηδενισμός «από το reset»</button><button id="logInstall" type="button" class="ghost-btn" disabled>Η ταινία τοποθετήθηκε / αλλάχτηκε τώρα</button><span id="logNote" class="log-note"></span></div>
      <p class="disclaimer" id="logInstallNote">—</p>
      <p class="disclaimer">Εκτιμήσεις από το ίδιο μοντέλο ρεύματος με το όριο (mA ανά LED της ρύθμισης), όχι μέτρηση. Τα «από πάντα» δεν μηδενίζονται ποτέ από τη σελίδα. Αποθήκευση στο <code>strip_stats.json</code> κάθε λεπτό και στην έξοδο.</p>
    </details>
  </div>
</section>
</div>

<div id="paneBridge" role="tabpanel" aria-labelledby="tabBridge" hidden>
<section class="bridge-grid">
    <div class="panel service-panel" id="servicePanel">
      <div class="panel-top"><div><div class="eyebrow">BRIDGE</div><h2>Αυτό το bridge</h2></div></div>
      <div class="diagnostic-grid" style="grid-template-columns:1fr">
        <div><span>Όνομα</span><strong id="bridgeName">—</strong></div>
        <div><span>MAC</span><strong id="bridgeMac">—</strong></div>
        <div><span>Bridge ID</span><strong id="bridgeIdField">—</strong></div>
        <div><span>IP</span><strong id="bridgeIp">—</strong></div>
        <div><span>Αποθηκευμένα pairing</span><strong id="bridgePairings">—</strong></div>
      </div>
      <div class="pair-hero" id="pairHero">
        <button id="btnPair" type="button">Pair (30 s)</button>
        <strong id="linkStatus">—</strong>
      </div>
      <p class="disclaimer">Όπως το κουμπί πάνω σε μια αληθινή Hue γέφυρα: πάτα Pair και μετά «Διαμόρφωση» στο Ambilight+hue της TV μέσα σε 30 s. Στο terminal το ίδιο κάνει το πλήκτρο <strong>p</strong>.</p>
      <div class="service-buttons">
        <button id="btnStart" type="button">Start</button>
        <button id="btnStop" type="button">Stop</button>
        <button id="btnRestart" type="button">Restart</button>
        <button id="btnExit" type="button">Έξοδος</button>
      </div>
      <p class="disclaimer" id="serviceNote">Έλεγχος κατάστασης συστήματος…</p>
    </div>

    <section class="panel strip-settings" id="stripSettingsPanel" aria-labelledby="stripSettingsTitle">
      <div class="panel-top"><div><div class="eyebrow">ΤΑΙΝΙΑ LED</div><h2 id="stripSettingsTitle">Ταινία LED &amp; ρεύμα</h2></div><a class="subtle-label" href="/ambi/settings">Σελίδα ρυθμίσεων (φωτεινότητα, θέσεις λαμπών) →</a></div>
      <form id="stripForm" novalidate>
        <div class="fields">
          <div><label for="setMaPerLed">mA ανά LED σε πλήρες λευκό</label>
            <input id="setMaPerLed" name="ma_per_led_full_white" type="number" min="1" max="200" step="0.1" inputmode="decimal" disabled>
            <span class="hint warn" id="hintMaPerLed">Το 45 μετρήθηκε στην ταινία αναφοράς (BTF WS2812B ECO 30 LED/m, ~44 mA). Άλλη ταινία: μέτρησέ την (README: «Measuring mA per LED»).</span></div>
          <div><label for="setBudget">Όριο ρεύματος ταινίας (mA)</label>
            <input id="setBudget" name="current_budget_ma" type="number" min="0" max="900" step="10" inputmode="numeric" disabled>
            <span class="hint">Σκληρό όριο, μόνο για τα LED. Τροφοδοτικό 900 mA → 800 αφήνει περιθώριο.</span></div>
          <div><label for="setLedCount">Πλήθος LED</label>
            <input id="setLedCount" name="led_count" type="number" min="1" max="1000" step="1" inputmode="numeric" disabled>
            <span class="hint">Ίδιο με το «Length» στο WLED.</span></div>
          <div><label for="setIdle">mA ανά LED όταν είναι σβηστό</label>
            <input id="setIdle" name="idle_ma_per_led" type="number" min="0" max="20" step="0.1" inputmode="decimal" disabled>
            <span class="hint">Το ρεύμα που τραβάει κάθε LED ακόμα και μαύρο.</span></div>
          <div><label for="setPeak">Έμφαση κορυφών (0–1)</label>
            <input id="setPeak" name="peak_emphasis" type="number" min="0" max="1" step="0.05" inputmode="decimal" disabled>
            <span class="hint">0 = παλιό όριο (όλα χαμηλώνουν μαζί). Πάνω από 0: χαμηλώνουν πρώτα τα LED ανάμεσα στις λάμπες και οι κορυφές μένουν φωτεινές. 1 = το πιο έντονο.</span></div>
          <div><label for="setRise">Χρόνος ανόδου (ms)</label>
            <input id="setRise" name="rise_ms" type="number" min="0" max="10000" step="10" inputmode="numeric" disabled>
            <span class="hint">Πόσο γρήγορα ανάβει ένα LED από 0 σε 100 % όσο παίζει η εικόνα. Το χαμήλωμα είναι άμεσο. 0 = χωρίς εξομάλυνση.</span></div>
          <div><label for="setFadeIn">Ομαλό άναμμα (ms)</label>
            <input id="setFadeIn" name="fade_in_ms" type="number" min="0" max="10000" step="100" inputmode="numeric" disabled>
            <span class="hint">Όταν ξεκινά η εικόνα της TV (ή μετά από Start / διακοπή), η ταινία ανάβει σιγά σιγά σε τόσο χρόνο. 0 = όπως πριν (rise_ms).</span></div>
          <div class="wb-field"><label>Ισορροπία λευκού (R / G / B)</label>
            <span class="wb-row"><input id="setWbR" name="wb_r" type="number" min="0.2" max="1" step="0.01" inputmode="decimal" disabled aria-label="R"><input id="setWbG" name="wb_g" type="number" min="0.2" max="1" step="0.01" inputmode="decimal" disabled aria-label="G"><input id="setWbB" name="wb_b" type="number" min="0.2" max="1" step="0.01" inputmode="decimal" disabled aria-label="B"></span>
            <span class="hint">Πόσο από κάθε χρώμα (0.2–1). Η ταινία βγάζει «κρύο» λευκό (μπλε) δίπλα στα LED της TV· για πιο ζεστό λευκό, κατέβασε το B (π.χ. 0.75) και λίγο το G (π.χ. 0.9). 1 / 1 / 1 = όπως πριν.</span></div>
          <div><label for="setWbKeep">Ισορροπία λευκού στα καθαρά χρώματα</label>
            <select id="setWbKeep" name="wb_keep_pure" disabled><option value="true">Κράτα τα καθαρά χρώματα</option><option value="false">Παντού</option></select>
            <span class="hint">«Κράτα»: η ισορροπία λευκού διορθώνει λευκά, γκρι και απαλά χρώματα, αλλά δεν αδυνατίζει ένα καθαρό μπλε, κόκκινο ή κίτρινο. «Παντού» = όπως στη v0.48.</span></div>
          <div><label for="setSmooth">Ομαλότητα μικρών αλλαγών (ms)</label>
            <input id="setSmooth" name="smooth_ms" type="number" min="0" max="2000" step="10" inputmode="numeric" disabled>
            <span class="hint">Οι μικρές αυξομειώσεις γίνονται σιγά σιγά (σαν το εργοστασιακό Ambilight)· όσο μεγαλύτερη η αλλαγή τόσο πιο γρήγορα, και μια αλλαγή σκηνής περνά αμέσως. Μεγαλύτερο = πιο «χαλαρό». 0 = όπως πριν.</span></div>
          <div><label for="setFadeOut">Ομαλό σβήσιμο (ms)</label>
            <input id="setFadeOut" name="fade_out_ms" type="number" min="0" max="10000" step="100" inputmode="numeric" disabled>
            <span class="hint">Όταν η εικόνα σκοτεινιάσει εντελώς (Ambilight off, τέλος εκπομπής), η ταινία σβήνει σιγά σιγά σε τόσο χρόνο. Οι κανονικές αλλαγές μένουν άμεσες. 0 = απότομα.</span></div>
          <div><label for="setDrift">Κυλιόμενες κορυφές: εύρος (LED ή auto)</label>
            <input id="setDrift" name="peak_drift_leds" type="text" inputmode="decimal" placeholder="auto" disabled>
            <span class="hint" id="hintDrift">Όταν το καρέ δεν χωράει στο όριο, οι κορυφές πηγαινοέρχονται ±τόσα LED γύρω από τις λάμπες, για να μην καταπονούνται πάντα τα ίδια LED. auto = η απόσταση δύο κορυφών (περνούν από όλα τα LED). 0 = σταθερές κορυφές. Δεν έχει δοκιμαστεί με το μάτι στην ταινία.</span></div>
          <div><label for="setDriftShape">Κυλιόμενες κορυφές: κίνηση</label>
            <select id="setDriftShape" name="peak_drift_shape" disabled><option value="triangle">Σταθερή ταχύτητα (τρίγωνο)</option><option value="sine">Ημίτονο (σταματά στις άκρες)</option></select>
            <span class="hint">Τρίγωνο: ίδια ταχύτητα παντού, ίσος χρόνος σε κάθε LED. Ημίτονο: όπως το v0.23, μένει περισσότερο στις άκρες της κίνησης.</span></div>
          <div><label for="setDriftPeriod">Κυλιόμενες κορυφές: ένας κύκλος (s)</label>
            <input id="setDriftPeriod" name="peak_drift_period_s" type="number" min="5" max="3600" step="5" inputmode="numeric" disabled>
            <span class="hint">Πόσα δευτερόλεπτα κάνει μια πλήρης κίνηση πέρα-δώθε. Αργά (π.χ. 300) για να μη φαίνεται στον τοίχο.</span></div>
          <div><label for="setPeakPoints">Πλήθος κορυφών (0 = στις λάμπες)</label>
            <input id="setPeakPoints" name="peak_points" type="number" min="0" max="12" step="1" inputmode="numeric" disabled>
            <span class="hint" id="hintPeakPoints">0 = οι κορυφές του ορίου είναι στις λάμπες, όπως πριν. 2–8 = τόσες κορυφές σε ίσες αποστάσεις πάνω στην ταινία, όχι στις λάμπες. Με τις κυλιόμενες κορυφές κινούνται όλες μαζί. Με χαμηλή «Έμφαση κορυφών» (0.2–0.3) γίνονται λίγο πιο φωτεινά σημεία που κινούνται αργά. Δρα μόνο όταν το καρέ δεν χωράει στο όριο. Δεν έχει δοκιμαστεί με το μάτι στην ταινία.</span></div>
          <div><label for="setLayout">Διάταξη λαμπών στην ταινία</label>
            <select id="setLayout" name="strip_layout" disabled><option value="tv">Όπως τις βλέπει η TV (v0.30)</option><option value="inset">Μέσα από τις άκρες (όπως ως τη v0.29)</option></select>
            <span class="hint" id="hintLayout">Όπως τις βλέπει η TV: οι κάτω λάμπες (Λ2–Λ4 με 5 λάμπες, όλες με 3) απλώνονται από άκρη σε άκρη της ταινίας, και στα ακριανά LED μπαίνει λίγο «χρώμα άκρης» (πλευρά της TV). Μέσα από τις άκρες: ακριβώς η v0.29 (θέση ακριανών, λάμπες στις άκρες, γωνίες).</span></div>
          <div id="fieldEdgeSource"><label for="setEdgeSource">Χρώμα άκρης: από πού</label>
            <select id="setEdgeSource" name="edge_source" disabled><option value="auto">Αυτόματα</option><option value="lamps">Λάμπες Λ1 / Λ5</option><option value="jointspace">JointSpace (κάτω ζώνη πλευράς)</option></select>
            <span class="hint" id="hintEdgeSource">Αυτόματα: με 5 λάμπες οι Λ1 / Λ5 (η TV τις δίνει στις πλευρές), με 3 λάμπες το JointSpace.</span></div>
          <div id="fieldEdgeLeds"><label for="setEdgeLeds">Χρώμα άκρης: πόσα LED (0–10)</label>
            <input id="setEdgeLeds" name="edge_leds" type="number" min="0" max="10" step="1" inputmode="numeric" disabled>
            <span class="hint">Σε τόσα LED από κάθε άκρη μπαίνει το χρώμα άκρης, πιο δυνατά στο πρώτο και λιγότερο προς τα μέσα. 0 = καθόλου.</span></div>
          <div id="fieldEdgeStrength"><label for="setEdgeStrength">Χρώμα άκρης: ένταση (0–1) <output id="setEdgeStrengthValue" for="setEdgeStrength"></output></label>
            <input id="setEdgeStrength" name="edge_strength" type="range" min="0" max="1" step="0.05" disabled>
            <span class="hint" id="hintEdgeStrength">Πόσο από το χρώμα άκρης παίρνει το πρώτο LED. 0.5 = μισό-μισό με τη λάμπα. 1 = μόνο το χρώμα άκρης.</span></div>
          <div id="fieldInset"><label for="setInset">Θέση ακριανών λαμπών (0–0.45)</label>
            <input id="setInset" name="lamp_inset" type="number" min="0" max="0.45" step="0.01" inputmode="decimal" disabled>
            <span class="hint" id="hintInset">Πόσο μέσα από κάθε άκρη της ταινίας μπαίνει η πρώτη και η τελευταία λάμπα. 0.25 = στα τέταρτα, 0 = στις άκρες. Πιο έξω από αυτές κρατιέται το χρώμα της ακριανής λάμπας.</span></div>
          <div><label for="setBlend">Μετάβαση χρωμάτων</label>
            <select id="setBlend" name="blend_mode" disabled><option value="smooth">Ομαλή ανάμειξη</option><option value="zones">Διακριτές ζώνες</option><option value="zones_mid">Ζώνες + ενδιάμεσες</option></select>
            <span class="hint" id="hintBlend">Ομαλή: το χρώμα αλλάζει σταδιακά από λάμπα σε λάμπα. Ζώνες: κάθε LED παίρνει την πιο κοντινή λάμπα. Ζώνες + ενδιάμεσες: ανάμεσα σε δύο λάμπες μια ζώνη με τη μείξη τους.</span></div>
          <div id="fieldSoftness"><label for="setSoftness">Απαλότητα ορίων (0–1) <output id="setSoftnessValue" for="setSoftness"></output></label>
            <input id="setSoftness" name="blend_softness" type="range" min="0" max="1" step="0.05" disabled>
            <span class="hint">Για τις ζώνες: 0 = κοφτά όρια, όπως πριν. Π.χ. 0.3 = κάθε ζώνη μένει ίδια στη μέση της και στα όρια γλιστρά στο επόμενο χρώμα (30 % του πλάτους). 1 = ίδιο με «Ομαλή ανάμειξη».</span></div>
          <div id="fieldEndLamps"><label for="setEndLamps">Λάμπες στις άκρες (5 λάμπες)</label>
            <select id="setEndLamps" name="end_lamps" disabled><option value="false">Όχι (όπως πριν)</option><option value="true">Ναι (Λ1 και Λ5 στις άκρες)</option></select>
            <span class="hint" id="hintEndLamps">Με 5 λάμπες: η Λ1 και η Λ5 (οι γωνίες της TV) πάνε στις άκρες της ταινίας, οι Λ2–Λ4 στις θέσεις των 3 λαμπών. Τότε δεν μπαίνουν γωνίες από το JointSpace και οι κορυφές του ορίου είναι μόνο στις Λ2–Λ4. Με 3 λάμπες δεν κάνει τίποτα.</span></div>
          <div id="fieldCorner"><label for="setCorner">Γωνίες (έξω από τις ακριανές λάμπες)</label>
            <select id="setCorner" name="corner_mode" disabled><option value="side">Μείξη με την πλευρά της TV</option><option value="lamp">Κρατά το χρώμα της λάμπας</option></select>
            <span class="hint" id="hintCorner">Μείξη: η άκρη παίρνει το ενδιάμεσο της ακριανής λάμπας και του κάτω χρώματος της αριστερής/δεξιάς πλευράς (JointSpace). Χωρίς JointSpace κρατά τη λάμπα. Θέλει «Θέση ακριανών λαμπών» πάνω από 0.</span></div>
          <div><label for="setLossHold">Χάθηκε το stream: κράτημα (ms)</label>
            <input id="setLossHold" name="loss_hold_ms" type="number" min="0" max="10000" step="100" inputmode="numeric" disabled>
            <span class="hint">Όταν η TV σταματήσει να στέλνει, η ταινία κρατά το τελευταίο καρέ τόσο, μετά σβήνει.</span></div>
          <div><label for="setLossFade">Χάθηκε το stream: σβήσιμο (ms)</label>
            <input id="setLossFade" name="loss_fade_ms" type="number" min="0" max="10000" step="100" inputmode="numeric" disabled>
            <span class="hint">Πόσο κρατά το σβήσιμο προς το μαύρο. 0 = κατευθείαν μαύρο.</span></div>
          <div><label for="setLossBlack">Χάθηκε το stream: μαύρο για (s)</label>
            <input id="setLossBlack" name="loss_black_s" type="number" min="0" max="3600" step="5" inputmode="numeric" disabled>
            <span class="hint">Τόσα δευτερόλεπτα στέλνει μαύρο, μετά σταματά και το WLED πάει στο δικό του preset (βάλε το preset του WLED σε off, βλ. README).</span></div>
        </div>
        <div class="summary" id="stripFormSummary">—</div>
        <div class="actions">
          <button id="saveStrip" type="submit" disabled>Αποθήκευση και εφαρμογή</button>
          <button id="resetStripForm" type="button" class="secondary" disabled>Αναίρεση αλλαγών</button>
          <button id="defaultsStrip" type="button" class="secondary" disabled>Προεπιλογές στη φόρμα</button>
        </div>
      </form>
      <p id="stripNote" role="status">Έλεγχος ρυθμίσεων…</p>
      <p class="disclaimer">Εφαρμόζονται αμέσως στο επόμενο frame και αποθηκεύονται στο <code>settings.json</code> (--state-dir). Οι τιμές εκτός ορίων διορθώνονται από το bridge. Το ρεύμα είναι εκτίμηση από αυτές τις τιμές, όχι μέτρηση.</p>
    </section>

    <details class="panel credentials-panel" id="credentialsPanel" open>
      <summary>Ρυθμίσεις σύνδεσης TV (JointSpace)</summary>
      <p>JointSpace credentials για τις τρεις εργοστασιακές πλευρές και τη μείξη στις γωνίες. Η IP της TV ορίζεται με <code>--tv-ip</code> στην εκκίνηση του bridge, όχι εδώ.</p>
      <form id="credentialsForm">
        <label for="tvUser">TV_USER</label>
        <input id="tvUser" name="username" type="text" autocomplete="username" maxlength="512" required disabled>
        <label for="tvPass">TV_PASS</label>
        <input id="tvPass" name="password" type="password" autocomplete="current-password" maxlength="512" required disabled>
        <button id="saveCredentials" type="submit" disabled>Αποθήκευση και εφαρμογή</button>
      </form>
      <p id="credentialsNote" role="status">Έλεγχος διαθέσιμων ρυθμίσεων…</p>
      <p class="disclaimer">Αποθηκεύονται ιδιωτικά σε αυτό το Linux (--state-dir) για τις επόμενες εκκινήσεις. Το password δεν εμφανίζεται στα diagnostics.</p>
    </details>
    <section class="panel identity-panel" id="identityPanel" aria-labelledby="identityTitle">
      <div class="panel-top"><div><div class="eyebrow">ΤΑΥΤΟΤΗΤΑ</div><h2 id="identityTitle">Όνομα, MAC &amp; λάμπες</h2></div></div>
      <p class="id-intro">Η TV αναγνωρίζει τη γέφυρα από τη MAC. Κάθε ταυτότητα κρατά τη δική της σύζευξη, οπότε με νέα MAC η TV βλέπει καινούργια γέφυρα (Pair + «Διαμόρφωση» από την αρχή) και η παλιά μένει όπως ήταν.</p>
      <div id="identityList" class="id-list" role="radiogroup" aria-label="Ταυτότητες bridge"></div>
      <form id="identityForm" class="id-form" novalidate>
        <div><label for="idName">Όνομα επιλεγμένης ταυτότητας</label>
        <div class="id-row"><input id="idName" type="text" maxlength="32" autocomplete="off" spellcheck="false" disabled><button id="idRename" type="submit" disabled>Μετονομασία</button></div>
        <span class="hint">Έως 32 λατινικοί χαρακτήρες, χωρίς &lt; &gt; &amp; " '.</span></div>
        <div><label for="idLamps">Λάμπες που δίνουμε στην TV</label>
        <div class="id-row"><select id="idLamps" disabled><option value="3">3</option><option value="5">5</option></select><button id="idLampsSave" type="button" disabled>Αποθήκευση</button></div>
        <span class="hint">Αλλαγή πλήθους = η TV πρέπει να ξανακάνει «Διαμόρφωση» για να δει τις νέες λάμπες.</span></div>
        <div class="id-row id-new"><button id="idCreate" type="button" class="secondary" disabled>+ Νέα ταυτότητα (νέα MAC)</button></div>
      </form>
      <div id="identityRestart" class="id-restart" role="status" hidden></div>
      <p id="identityNote" class="id-note" role="status">Έλεγχος…</p>
    </section>
    <section class="panel discovery-panel" id="discoveryPanel" aria-labelledby="discoveryTitle">
      <div class="panel-top"><div><div class="eyebrow">ΑΝΑΚΑΛΥΨΗ ΣΤΟ ΔΙΚΤΥΟ</div><h2 id="discoveryTitle">Ποιος ρωτάει</h2></div><button id="discRefresh" type="button" class="ghost-btn">Ανανέωση</button></div>
      <p class="id-intro">Συσκευές και προγράμματα που έψαξαν γέφυρα (SSDP) ή διάβασαν το <code>description.xml</code> από την εκκίνηση, με ολόκληρο το User-Agent. Τα πλήρη γεγονότα είναι και στο <code>ambi_bridge.log</code> του --state-dir.</p>
      <label class="disc-switch"><input type="checkbox" id="discHueOnly" disabled> <span><strong>Απάντα μόνο σε αναζητήσεις Hue</strong><br>Αγνοεί π.χ. την αναζήτηση cast ενός browser (<code>dial-multiscreen</code>). Όπως μια πραγματική γέφυρα Hue. Το mDNS δεν επηρεάζεται. Αν η TV σταματήσει να βρίσκει τη γέφυρα, κλείσ' το.</span></label>
      <div id="discList" class="disc-list" aria-live="off"></div>
      <p id="discNote" class="id-note" role="status">Έλεγχος…</p>
    </section>
    <section class="panel identity-panel access-panel" id="accessPanel" aria-labelledby="accessTitle">
      <div class="panel-top"><div><div class="eyebrow">ΠΡΟΣΒΑΣΗ</div><h2 id="accessTitle">Αλλαγές από άλλες συσκευές</h2></div></div>
      <p class="id-intro">Χωρίς login, ρυθμίσεις και Start/Stop αλλάζουν μόνο από τον browser αυτού του Linux (http://127.0.0.1). Με login, και από κινητό ή άλλο υπολογιστή μετά από «Σύνδεση» πάνω δεξιά. Το όνομα και ο κωδικός ορίζονται μόνο από εδώ, στον browser του ίδιου Linux.</p>
      <p class="id-intro warn">Η σελίδα είναι απλό HTTP: ο κωδικός περνά μη κρυπτογραφημένος στο οικιακό δίκτυο. Βάλε κωδικό που δεν χρησιμοποιείς αλλού.</p>
      <form id="accessForm" class="id-form" novalidate hidden>
        <div><label for="accUser">Όνομα χρήστη</label>
        <div class="id-row"><input id="accUser" type="text" maxlength="32" autocomplete="username" spellcheck="false"></div>
        <span class="hint">1–32 λατινικοί χαρακτήρες, χωρίς κενά.</span></div>
        <div><label for="accPass">Νέος κωδικός (τουλάχιστον 8)</label>
        <div class="id-row"><input id="accPass" type="password" maxlength="128" autocomplete="new-password"></div></div>
        <div><label for="accPass2">Ξανά ο κωδικός</label>
        <div class="id-row"><input id="accPass2" type="password" maxlength="128" autocomplete="new-password"></div></div>
        <div class="id-row id-new"><button id="accSave" type="submit">Αποθήκευση login</button><button id="accClear" type="button" class="secondary">Αφαίρεση login</button></div>
      </form>
      <div class="id-row" id="accLogoutRow" hidden><button id="accLogout" type="button" class="secondary">Αποσύνδεση</button></div>
      <p id="accessNote" class="id-note" role="status">Έλεγχος…</p>
    </section>
</section>
</div>
<dialog id="loginDialog" class="login-dialog" aria-labelledby="loginTitle">
  <form id="loginForm" method="dialog" novalidate>
    <h2 id="loginTitle">Σύνδεση</h2>
    <p>Για αλλαγές ρυθμίσεων και Start/Stop από αυτή τη συσκευή. Μένεις συνδεδεμένος <span id="loginDays">7</span> μέρες.</p>
    <label for="loginUser">Όνομα χρήστη</label>
    <input id="loginUser" type="text" autocomplete="username" spellcheck="false" maxlength="32">
    <label for="loginPass">Κωδικός</label>
    <input id="loginPass" type="password" autocomplete="current-password" maxlength="128">
    <p class="err" id="loginErr" role="alert"></p>
    <p>Απλό HTTP: ο κωδικός περνά μη κρυπτογραφημένος στο οικιακό δίκτυο.</p>
    <div class="row"><button id="loginCancel" type="button" class="secondary">Άκυρο</button><button id="loginSubmit" type="submit">Σύνδεση</button></div>
  </form>
</dialog>
<button id="virtualLampsBackdrop" type="button" class="lamps-backdrop" aria-label="Κλείσιμο πάνελ εικονικών λαμπών" hidden></button>
<aside id="virtualLampsPanel" class="lamps-drawer" role="dialog" aria-modal="true" aria-labelledby="virtualLampsTitle" aria-hidden="true" tabindex="-1" hidden>
  <div class="lamps-drawer-heading">
    <div><div class="eyebrow">RAW ENTERTAINMENT STREAM</div><h2 id="virtualLampsTitle">Οι 3 εικονικές Hue λάμπες</h2><p>Bottom Left · Bottom Middle · Bottom Right</p></div>
    <button id="virtualLampsClose" class="lamps-close-btn" type="button" aria-label="Κλείσιμο πάνελ">✕</button>
  </div>
  <div class="drawer-source"><span class="source-dot" id="drawerHueDot"></span><span id="drawerHueStatus">Χωρίς σύνδεση</span><span id="drawerHueFps">— fps</span></div>
  <p class="drawer-explanation">Τα <strong>3 HueStream κανάλια</strong> πριν από την παρεμβολή της ταινίας. Οι τιμές <strong>XY + bri / raw16</strong> παραμένουν όπως ελήφθησαν. Το RGB εισόδου είναι η προσεγγιστική μετατροπή της bridge· το RGB οθόνης περιλαμβάνει τη φωτεινότητα προβολής.</p>
  <div id="virtualLampsDetail" class="drawer-lamps" aria-live="off"></div>
  <div class="drawer-foot">Όταν η TV σταματήσει το Hue Entertainment stream, οι παλιές τιμές αποσύρονται από τη ζωντανή προβολή. Δεν στέλνονται εντολές προς την τηλεόραση.</div>
</aside>
<footer><span>AMBILIGHT LAB · 4TH SIDE PROJECT</span><span id="footVersion">ambi-bridge-cpp monitor dashboard</span></footer>
</main>
<script>
'use strict';
/* Relative Cast profile: docs/COLOR-MODEL.md.
 * The first four legacy helpers remain for the independent v1.4 current proxy.
 * Display mixing uses jointspace/hueWorking and never writes source data. */
(() => {
  const valid = c => Array.isArray(c) && c.length === 3 && c.every(Number.isFinite);
  const clip = value => Math.min(255, Math.max(0, value));
  const linear = c => valid(c) ? c.map(clip) : null;
  function srgbToLinear(color) {
    if (!valid(color)) return null;
    return color.map(value => {
      const c = clip(value) / 255;
      return 255 * (c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4);
    });
  }
  function toScreen(color, gain = 1) {
    if (!valid(color) || !Number.isFinite(gain) || gain < 0) return null;
    const scaled = color.map(value => clip(value) * gain);
    // Preserve channel ratios if viewing gain pushes the peak above full scale.
    const peakScale = Math.max(1, ...scaled.map(value => value / 255));
    return scaled.map(value => {
      const c = value / (255 * peakScale);
      const encoded = c <= .0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - .055;
      return Math.round(255 * encoded);
    });
  }
  function hueLinear(lamp, colorspace) {
    if (!valid(lamp?.rgb)) return null;
    // RGB HueStream payloads are direct levels; XY/bri rgb is the bridge's
    // existing gamma-encoded estimate. Do not silently apply bri^2 here.
    return colorspace === 0 ? linear(lamp.rgb) : srgbToLinear(lamp.rgb);
  }
  // Fixed Cast profile: measured native white = 23, never frame-normalized.
  const jointspace = c => valid(c) ? c.map(v => clip(Math.max(0,v) * 255 / 23)) : null;
  // v0.33: the TV's side zones as the strip treats them (settings corner_js_curve /
  // corner_js_full_scale; linear / 23 = the line above). The Hue profile below keeps 23.
  const jsCfg = {curve:'linear', full:23};
  const setJointspace = (curve, full) => { jsCfg.curve = curve === 'srgb' ? 'srgb' : 'linear'; jsCfg.full = Number.isFinite(full) && full >= 1 ? full : 23; };
  const jointspaceSide = c => {
    if (!valid(c)) return null;
    const lin = c.map(v => clip(Math.max(0,v) * 255 / jsCfg.full));
    return jsCfg.curve === 'srgb' ? srgbToLinear(lin) : lin;
  };
  const anchors = [
    [[20176,21194,4112],[0,0,0]],
    [[17416,17830,19789],[23,23,23]],
    [[45174,20106,19789],[23,0,0]],
    [[15914,42078,19789],[0,23,0]],
    [[9988,3133,19789],[0,0,23]],
    [[11016,17030,19789],[0,23,23]],
    [[16963,6497,19789],[23,0,23]],
    [[26771,33925,19789],[23,23,0]],
    [[20093,21100,4883],[1,1,1]],
    [[17416,17830,17733],[18,18,18]],
    [[17436,17830,17476],[18,18,18]],
    [[21369,21273,4626],[1,0,0]],
    [[45174,20106,17733],[19,0,0]],
    [[45174,20106,17476],[19,0,0]],
    [[20098,22712,4883],[0,1,0]],
    [[16031,41991,17733],[0,19,0]],
    [[16056,41972,17476],[0,19,0]],
    [[19342,19869,4626],[0,0,1]],
    [[9988,3133,17733],[0,0,19]],
    [[9988,3133,17476],[0,0,19]]
  ];
  // Fixed metric in xy + squared brightness, not a per-lamp normalization.
  // IDW reproduces measured anchors; arbitrary mixtures are provisional.
  const feature = raw => [raw[0]/65535, raw[1]/65535, (raw[2]/19789)**2];
  function hueWorking(lamp, colorspace) {
    if (colorspace === 0) return linear(lamp?.rgb); // uncalibrated RGB fallback
    if (colorspace !== 1) return null;
    const raw = lamp?.raw16;
    if (!valid(raw) || raw.some(v=>v<0 || v>65535) || raw[1]===0) return null;
    const point=feature(raw);
    let total=0, result=[0,0,0];
    for (const [input,output] of anchors) {
      const f=feature(input);
      const d2=f.reduce((sum,v,k)=>sum+(v-point[k])**2,0);
      if (d2<1e-16) return jointspace(output);
      const weight=1/(d2*d2);
      total+=weight;
      result=result.map((v,k)=>v+weight*output[k]);
    }
    return jointspace(result.map(v=>v/total));
  }
  const model = Object.freeze({linear, srgbToLinear, toScreen, hueLinear,
    jointspace, jointspaceSide, setJointspace, hueWorking, profile:'cast-relative-23-idw-v1', anchors});
  if (typeof module !== 'undefined' && module.exports) module.exports = model;
  else globalThis.AmbilightColor = model;
})();

</script>
<script>
'use strict';
/* Ported from the old Python prototype's web/app.js (v1.5), adapted for
 * this C++ bridge: transport is SSE on /events like the original (since
 * v6; earlier versions polled /api/state every 250 ms, which was visibly
 * laggier), and a "Bridge" panel (identity, pairing count, systemd
 * Stop/Restart) is merged in at the bottom. Everything else - the four
 * view modes, the drawer, calibration, freshness charts, diagnostics copy
 * button, credentials form - is unchanged from the original. */
(() => {
  const $ = id => document.getElementById(id);
  const Color = globalThis.AmbilightColor;
  const edges = {top:$('topEdge'),left:$('leftEdge'),right:$('rightEdge'),bottom:$('bottomEdge')};
  const drawer = $('virtualLampsPanel');
  const drawerBackdrop = $('virtualLampsBackdrop');
  const drawerDetails = $('virtualLampsDetail');
  const drawerToggle = $('virtualLampsButton');
  let drawerOpen = false;
  const statusNames = {live:'Ζωντανό',demo:'Demo',inactive:'Ανενεργό',waiting:'Αναμονή',stale:'Stale',disabled:'Απενεργοποιημένο',not_configured:'Χωρίς credentials',unavailable:'Χωρίς σύνδεση',off:'Κλειστό (επιλογή)'};
  // v10: two independent choices instead of four combined modes.
  //   view.sides : 'zones' (JointSpace zones as received, 3/8/3)
  //                'leds'  (estimated TV LED positions, 9/28/9)
  //   view.bottom: 'lamps' (the Hue lamps as received: 3 or 5, per --lamps)
  //                'strip' (v0.12: the strip exactly as the bridge sends
  //                         it to WLED, from state.output.leds; the old
  //                         browser-side simulation and 'diy' are gone)
  const sideNotes = {
    zones:'Πλευρές: οι ζώνες της TV όπως έρχονται από το JointSpace.',
    leds:'Πλευρές: εκτιμώμενες θέσεις των LED της TV (9/28/9), με παρεμβολή ανάμεσα στις ζώνες.'
  };
  const bottomNotes = {
    lamps:'Κάτω: οι Hue λάμπες όπως τις στέλνει η TV, χωρίς παρεμβολή.',
    strip:'Κάτω: η ταινία LED ακριβώς όπως τη στέλνει το bridge στο WLED (με τη μετάβαση χρωμάτων, τις γωνίες και το όριο ρεύματος, χωρίς βαθμονόμηση χρώματος).',
    zones:'Κάτω: οι ζώνες της ταινίας, μία θέση ανά ζώνη, όπως οι πλευρές δείχνουν 3/8/3. Τις ορίζουν η «Μετάβαση χρωμάτων» και οι «Γωνίες» (καρτέλα Bridge): γωνιακή ζώνη όταν η γωνία αναμειγνύεται με την πλευρά, ενδιάμεση ζώνη με «Ζώνες + ενδιάμεσες». Μόνο προβολή, δεν στέλνεται.',
    leds:'Κάτω: 28 θέσεις, όσες και η πάνω πλευρά της TV, για να συγκρίνεις με τα ίδια μάτια. Οι λάμπες μπαίνουν στις θέσεις της ρύθμισης «Θέση ακριανών λαμπών», με παρεμβολή ανάμεσα, χωρίς όριο ρεύματος. Μόνο προβολή, δεν στέλνεται.'
  };
  const view = {sides:'zones', bottom:'lamps', fill:true, glow:true};
  let stripServerInset=null;  // lamp_inset from /api/settings/strip, until a frame carries it
  let stripServerModes={};    // v0.18 blend_mode/corner_mode from /api/settings/strip, same reason
  try{
    const saved=JSON.parse(localStorage.getItem('ambi.view')||'{}');
    if(['zones','leds'].includes(saved.sides))view.sides=saved.sides;
    if(['lamps','zones','leds','strip'].includes(saved.bottom))view.bottom=saved.bottom;
    else if(saved.bottom==='diy'||saved.bottom==='compare')view.bottom='strip';
    if(typeof saved.fill==='boolean')view.fill=saved.fill;
    if(typeof saved.glow==='boolean')view.glow=saved.glow;
  }catch(e){}
  function saveView(){try{localStorage.setItem('ambi.view',JSON.stringify(view));}catch(e){}}
  const histories={js:[],hue:[]};
  let state = null;
  let lastEventAt = 0;
  let connected = false;
  const pageUpdates=[];  // v0.23: arrival times of /events updates (last 5 s), for the chip popover
  const params = {gain:1};

  function hueDisplay(lamp,s){return s?.demo && !lamp?.raw16 ? Color.srgbToLinear(lamp?.rgb):Color.hueWorking(lamp,s?.sources?.hue?.colorspace);}
  function formatAge(ms){return ms == null ? '—' : ms < 1000 ? `${ms} ms` : `${(ms/1000).toFixed(1)} s`;}
  function rgbCss(c) {return c && c.length===3 ? `rgb(${c.map(x=>Math.max(0,Math.min(255,Math.round(x)))).join(',')})` : null;}
  function hex(c) {return c ? '#' + c.map(x=>Math.max(0,Math.min(255,Math.round(x))).toString(16).padStart(2,'0')).join('').toUpperCase() : '—';}
  function clamp(n,a,b){return Math.min(b,Math.max(a,n));}
  function isColor(c){return Array.isArray(c)&&c.length===3&&c.every(Number.isFinite);}
  // v0.16/v0.18: bottom "28" view - the same rule as the bridge's
  // strip_curve(): lamps at inset..1-inset, blend smooth / zones /
  // zones_mid, corners held or mixed with the side colour at that corner.
  // Anchors (position 0..1, colour) in strip order: corner, lamp, [mid], lamp, ..., corner.
  // keepPlainCorners=false drops a corner that only repeats its lamp (the zones view).
  function stripAnchors(samples, inset, blend, cornerL, cornerR, keepPlainCorners, endLamps){
    if(!samples.length||samples.some(c=>!isColor(c))) return [];
    inset=clamp(Number(inset)||0,0,0.45);
    const n=samples.length, mix=(a,b)=>a.map((v,k)=>(v+b[k])/2);
    // v0.24 end_lamps: lamps 1 and 5 at the very ends, 2..4 at inset / 50 % / 1-inset, no corner anchors
    const ends=Boolean(endLamps)&&n===5&&inset>0;
    const pos=ends?[0,inset,0.5,1-inset,1]:n===1?[0.5]:samples.map((_,j)=>inset+(1-2*inset)*j/(n-1));
    const anchors=[];
    if(ends){samples.forEach((c,j)=>{if(j>0&&blend==='zones_mid')anchors.push([(pos[j-1]+pos[j])/2,mix(samples[j-1],c),'mid']);anchors.push([pos[j],c,'lamp']);});return anchors;}
    if(inset>0&&(keepPlainCorners||isColor(cornerL)))anchors.push([0,isColor(cornerL)?mix(cornerL,samples[0]):samples[0],'corner']);
    samples.forEach((c,j)=>{if(j>0&&blend==='zones_mid')anchors.push([(pos[j-1]+pos[j])/2,mix(samples[j-1],c),'mid']);anchors.push([pos[j],c,'lamp']);});
    if(inset>0&&(keepPlainCorners||isColor(cornerR)))anchors.push([1,isColor(cornerR)?mix(samples[n-1],cornerR):samples[n-1],'corner']);
    return anchors;
  }
  // v0.30 strip_layout "tv": the bottom lamps (Λ2..Λ4 with 5 lamps, all with 3) evenly from end
  // to end of the strip - the bridge's tv_curve(). No corner anchors; the edge colour is mixed in
  // afterwards (edgeMix). edge = {leds, share, left, right, ledCount} or null.
  function tvAnchors(samples, blend, edge, withEdgeCells){
    if(!samples.length||samples.some(c=>!isColor(c))) return [];
    const b=samples.length===5?samples.slice(1,4):samples, nb=b.length, mix=(a,c)=>a.map((v,k)=>(v+c[k])/2);
    const pos=nb===1?[0.5]:b.map((_,j)=>j/(nb-1)), anchors=[];
    b.forEach((c,j)=>{if(j>0&&blend==='zones_mid')anchors.push([(pos[j-1]+pos[j])/2,mix(b[j-1],c),'mid']);anchors.push([pos[j],c,'lamp']);});
    if(withEdgeCells&&edge&&edge.leds>0&&edge.share>0){
      const lerp=(a,t,w)=>a.map((v,k)=>v+(t[k]-v)*w);
      if(isColor(edge.left))anchors.unshift([0,lerp(anchors[0][1],edge.left,edge.share),'corner']);
      if(isColor(edge.right))anchors.push([1,lerp(anchors[anchors.length-1][1],edge.right,edge.share),'corner']);
    }
    return anchors;
  }
  // LED i of the real strip gets share*(E-i)/E of the edge colour (the bridge's apply_edges); a view
  // with fewer cells uses the same rule at its position x: w = share*max(0, 1 - x*(L-1)/E).
  function edgeMix(colors, edge){
    if(!edge||!(edge.leds>0)||!(edge.share>0)||colors.length<2)return colors;
    const L=Math.max(2,edge.ledCount||colors.length), E=Math.min(edge.leds,Math.floor(L/2)), n=colors.length;
    if(E<=0)return colors;
    return colors.map((c,i)=>{
      let out=c;
      const blendTo=(t,w)=>{if(isColor(t)&&w>0)out=out.map((v,k)=>v+(t[k]-v)*w);};
      blendTo(edge.left,edge.share*Math.max(0,1-(i/(n-1))*(L-1)/E));
      blendTo(edge.right,edge.share*Math.max(0,1-((n-1-i)/(n-1))*(L-1)/E));
      return out;
    });
  }
  function interpolateInset(samples, count, inset, blend, cornerL, cornerR, softness, endLamps){
    return blendAnchorsJs(stripAnchors(samples,inset,blend,cornerL,cornerR,true,endLamps),count,blend,softness);
  }
  function blendAnchorsJs(anchors, count, blend, softness){
    if(!anchors.length) return [];
    const result=[];
    const soft=clamp(Number(softness)||0,0,1);
    for(let i=0;i<count;i++){
      const x=count===1?0.5:i/(count-1);
      if((blend==='zones'||blend==='zones_mid')&&soft>0&&anchors.length>1){
        // v0.23 (NEXT 9): soft zone borders, the bridge's rule
        let a=0;while(a+2<anchors.length&&x>anchors[a+1][0])a++;
        const [p,ca]=anchors[a],[q,cb]=anchors[a+1],m=(p+q)/2,h=soft*(q-p)/2;
        let w=0;
        if(q<=p)w=x>p?1:0;else if(x>=m+h)w=1;else if(x>m-h)w=h>0?(x-(m-h))/(2*h):(x>m?1:0);
        if(x<p)w=0;
        result.push(ca.map((v,k)=>v*(1-w)+cb[k]*w));continue;
      }
      if(blend==='zones'||blend==='zones_mid'){
        let best=0,bd=1e9;anchors.forEach(([p],k)=>{const d=Math.abs(x-p);if(d<bd-1e-12){bd=d;best=k;}});
        result.push(anchors[best][1].slice());continue;
      }
      if(anchors.length===1){result.push(anchors[0][1].slice());continue;}
      let a=0;while(a+2<anchors.length&&x>anchors[a+1][0])a++;
      const [p,ca]=anchors[a],[q,cb]=anchors[a+1],w=q>p?clamp((x-p)/(q-p),0,1):0;
      result.push(ca.map((v,k)=>v*(1-w)+cb[k]*w));
    }
    return result;
  }
  function interpolate(samples, count){
    if(!samples.length||samples.some(c=>!isColor(c))) return [];
    if(count<=1) return [samples[0]];
    const result=[];
    for(let i=0;i<count;i++){
      const p=(i/(count-1))*(samples.length-1), a=Math.floor(p), b=Math.min(samples.length-1,a+1), w=p-a;
      result.push(samples[a].map((v,k)=>v*(1-w)+samples[b][k]*w));
    }
    return result;
  }
  function setPixels(container,colors){
    if(container.children.length!==colors.length){
      const items=colors.map(()=>{const el=document.createElement('div');el.className='pixel';return el;});
      container.replaceChildren(...items);
    }
    colors.forEach((rgb,i)=>{
      const el=container.children[i];
      const shown=Color.toScreen(rgb,params.gain);
      const c=rgbCss(shown);el.classList.toggle('active',Boolean(c));
      el._shown=c?shown:null;
      el.style.backgroundColor=c||'#303d52';
      if(c)el.style.setProperty('--pixel-color',c);
      el.title=c?`RGB οθόνης: ${shown.join(', ')} · υπολογισμένη προβολή`:'';
    });
  }
  function setSource(prefix,s){
    const online = connected && (s.status==='live'||s.status==='demo');
    $(prefix+'Dot').classList.toggle('live',online);
    const errorNames={authentication_failed:'Απόρριψη credentials (401)',forbidden:'Απαγορεύεται (403)',network_unavailable:'TV μη προσβάσιμη',network_timeout:'Η TV δεν απάντησε εγκαίρως (timeout)',invalid_response:'Μη έγκυρα δεδομένα TV',invalid_address:'Μη έγκυρη IP τηλεόρασης',request_failed:'Αποτυχία ανάγνωσης TV',low_memory:'Σε αναμονή: λίγη μνήμη στο ESP (προστασία)'};
    const oldSession=connected&&prefix==='hue'&&s.old_session&&!online;
    $(prefix+'Status').textContent = !connected ? 'Αποσύνδεση' : oldSession ? 'Παλιά σύνδεση' : (errorNames[s.error]||statusNames[s.status]||s.status);
    $(prefix+'Fps').textContent = online ? (Number(s.fps)||0).toFixed(1) : '—';
    $(prefix+'Freshness').textContent = online ? 'Ροή δειγμάτων' : 'Κενό σήματος';
    $(prefix+'ChipDot').classList.toggle('live',online);
    $(prefix+'ChipRate').textContent = online ? `${(Number(s.fps)||0).toFixed(prefix==='hue'?0:1)}/s` : oldSession ? 'παλιά σύνδεση' : (connected?'εκτός':'—');
    $(prefix+'ChipDot').classList.toggle('warn',Boolean(oldSession));
    if(prefix==='hue'){
      const lock=connected&&s.tv_dtls_lock&&!online?s.tv_dtls_lock:null;
      $('tvLockBanner').hidden=!lock;
      if(lock){
        const why=lock.reason==='timeout'?'Η TV ζήτησε ροή (stream.active) αλλά δεν ολοκληρώθηκε DTLS handshake μέσα σε 3 s.':'Το DTLS handshake της TV απέτυχε'+(lock.detail?` (${lock.detail})`:'')+'.';
        const proof=lock.confirmed?' Επιβεβαιώθηκε: η TV ζήτησε μετά LightGroup.':'';
        const mins=Math.floor((Number(lock.age_s)||0)/60);
        $('tvLockText').textContent=`${why}${proof} Η TV δεν ξαναδοκιμάζει ροή μέχρι να ξαναξεκινήσει η υπηρεσία Hue της: κάνε restart της TV (βγάλε την από το ρεύμα για λίγα δευτερόλεπτα). Το κλείσιμο/άνοιγμα του Ambilight+hue δεν αρκεί.${mins>0?` Κλειδωμένη εδώ και ${mins} λεπτά.`:''}`;
      }
      $('oldSessionBanner').hidden=!oldSession;
      if(oldSession)$('oldSessionBanner').dataset.ip=s.old_session.ip||'';
    }
  }
  function recordFreshness(now=Date.now()) {
    for(const [prefix,key] of [['js','jointspace'],['hue','hue']]) {
      const source=state?.sources?.[key];
      const ok=connected && ['live','demo'].includes(source?.status) && Number.isFinite(source.age_ms) && source.age_ms>=0;
      const h=histories[prefix];
      h.push([now,ok?source.age_ms:null]);
      while(h.length && h[0][0]<now-30000)h.shift();
      const max=Math.max(200,...h.map(p=>p[1]??0));
      let path='',pen=false;
      for(const [t,value] of h){
        if(value===null){pen=false;continue;}
        const x=300*(t-(now-30000))/30000,y=54-48*value/max;
        path+=`${pen?'L':'M'}${x.toFixed(1)},${y.toFixed(1)} `;pen=true;
      }
      $(prefix+'ChartLine').setAttribute('d',path);
      $(prefix+'Chart').dataset.samples=String(h.filter(p=>p[1]!==null).length);
      $(prefix+'Chart').dataset.gaps=String(h.filter(p=>p[1]===null).length);
      $(prefix+'Chart').setAttribute('aria-label',`Ηλικία δείγματος τελευταίων 30 δευτερολέπτων, 0–${Math.ceil(max)} ms. Τα κενά είναι διακοπές. Δεν είναι RTT.`);
      $(prefix+'ChartScale').textContent=`0–${Math.ceil(max)} ms · ηλικία δείγματος`;
    }
  }
  function createDrawerCards(n){
    if(drawerDetails.children.length===n)return;
    const cards=[];
    for(let i=0;i<n;i++){
      const card=document.createElement('article');card.className='drawer-lamp';
      const top=document.createElement('div');top.className='drawer-lamp-top';
      const sw=document.createElement('div');sw.className='drawer-lamp-swatch';
      const heading=document.createElement('div');heading.className='drawer-lamp-heading';
      const name=document.createElement('strong');name.className='drawer-lamp-name';
      const ident=document.createElement('span');ident.className='drawer-lamp-id';
      const flag=document.createElement('span');flag.className='drawer-lamp-flag';
      heading.append(name,ident);top.append(sw,heading,flag);
      const details=document.createElement('dl');details.className='drawer-lamp-metrics';
      for(const [key,label] of [['hex','HEX εισόδου (εκτίμηση)'],['rgb','RGB εισόδου (εκτίμηση)'],['display','RGB οθόνης'],['xy','XY (όπως ελήφθησαν)'],['bri','Brightness (8-bit)'],['raw','Raw data (16-bit)'],['age','Ηλικία δείγματος']]){
        const row=document.createElement('div');row.className='drawer-metric';
        const dt=document.createElement('dt');dt.textContent=label;
        const dd=document.createElement('dd');dd.dataset.metric=key;dd.textContent='—';
        row.append(dt,dd);details.append(row);
      }
      card.append(top,details);cards.push(card);
    }
    drawerDetails.replaceChildren(...cards);
  }
  function updateDrawer(s){
    const bottom=Array.isArray(s?.bottom)?s.bottom:[];
    const n=bottom.length||3;
    createDrawerCards(n);
    const hue=s?.sources?.hue||{};
    const signalOk=connected && (hue.status==='live'||hue.status==='demo');
    const lampsLive=bottom.filter(l=>signalOk && isColor(l?.rgb)).length;
    $('lampButtonCount').textContent=`${lampsLive}/${n}`;
    $('virtualLampsTitle').textContent=`Οι ${n} εικονικές Hue λάμπες`;
    $('drawerHueStatus').textContent=connected ? (statusNames[hue.status]||hue.status||'Αναμονή') : 'Αποσυνδεδεμένο';
    $('drawerHueFps').textContent=signalOk ? `${(Number(hue.fps)||0).toFixed(1)} fps` : '— fps';
    $('drawerHueDot').classList.toggle('live',signalOk);
    for(let i=0;i<n;i++){
      const l=bottom[i]||{};
      const card=drawerDetails.children[i];
      const color=(signalOk && isColor(l.rgb)) ? l.rgb : null;
      const display=color?Color.toScreen(hueDisplay(l,s),params.gain):null;
      card.classList.toggle('lamp-inactive',!color);
      card.querySelector('.drawer-lamp-swatch').style.backgroundColor=rgbCss(display)||'#303d52';
      card.querySelector('.drawer-lamp-name').textContent=l.name||`Lamp ${i+1}`;
      card.querySelector('.drawer-lamp-id').textContent=`Hue light #${String(l.id||i+1)}`;
      const tag=card.querySelector('.drawer-lamp-flag');
      tag.textContent=color ? (s?.demo?'DEMO':'LIVE') : (connected?(statusNames[hue.status]||'NO DATA'):'OFFLINE');
      tag.classList.toggle('lit',!!color);
      const set=(key,value)=>{card.querySelector(`[data-metric="${key}"]`).textContent=value;};
      set('hex',color ? hex(color):'—');
      set('rgb',color ? color.map(x=>Math.round(x)).join(', '):'—');
      set('display',display?display.join(', '):'—');
      const xy=Array.isArray(l.xy)&&l.xy.length===2&&l.xy.every(Number.isFinite)?l.xy:null;
      set('xy',color&&xy ? xy.map(v=>v.toFixed(5)).join(' / '):'—');
      set('bri',color&&Number.isFinite(l.bri8)?l.bri8.toFixed(2):'—');
      const raw=Array.isArray(l.raw16)&&l.raw16.length===3?l.raw16:null;
      set('raw',color&&raw ? raw.join(' / '):'—');
      set('age',color?formatAge(l.age_ms):'—');
    }
  }
  function setDrawerOpen(open){
    drawerOpen=Boolean(open);
    drawer.hidden=!drawerOpen;
    drawerBackdrop.hidden=!drawerOpen;
    drawer.setAttribute('aria-hidden',String(!drawerOpen));
    drawerToggle.setAttribute('aria-expanded',String(drawerOpen));
    document.body.classList.toggle('lamp-panel-open',drawerOpen);
    if(drawerOpen){updateDrawer(state);$('virtualLampsClose').focus();}
    else drawerToggle.focus();
  }
  drawerToggle.addEventListener('click',()=>setDrawerOpen(!drawerOpen));
  $('virtualLampsClose').addEventListener('click',()=>setDrawerOpen(false));
  drawerBackdrop.addEventListener('click',()=>setDrawerOpen(false));
  document.addEventListener('keydown',e=>{
    if(!drawerOpen)return;
    if(e.key==='Escape'){e.preventDefault();setDrawerOpen(false);return;}
    if(e.key==='Tab'){
      const focusables=[...drawer.querySelectorAll('button:not([disabled])')];
      if(!focusables.length)return;
      const first=focusables[0],last=focusables[focusables.length-1];
      if(e.shiftKey&&document.activeElement===first){e.preventDefault();last.focus();}
      else if(!e.shiftKey&&document.activeElement===last){e.preventDefault();first.focus();}
    }
  });
  // ---- v10: estimated picture in the middle of the TV ----------------------
  // Not the real picture: every pixel of a 32x18 canvas is an inverse-
  // distance-weighted (1/d^2) mix of the edge colours currently shown, mixed
  // in linear RGB, then Color.toScreen() like every edge pixel (so the
  // brightness slider applies). CSS blurs it. Smoothed 0.35/frame; reset at
  // once when the data goes away, so old colours never pose as live.
  const fillCanvas=$('screenFill'), fillCtx=fillCanvas.getContext('2d');
  const FW=fillCanvas.width, FH=fillCanvas.height;
  const fillImage=fillCtx.createImageData(FW,FH);
  let fillPrev=null;
  function drawScreenFill(top,left,right,bottom){
    const pts=[];
    const add=(arr,pos)=>arr.forEach((c,i)=>{if(isColor(c))pts.push([...pos(i,arr.length),c]);});
    add(top,(i,n)=>[(i+.5)/n,0]);
    add(bottom,(i,n)=>[(i+.5)/n,1]);
    add(left,(i,n)=>[0,(i+.5)/n]);
    add(right,(i,n)=>[1,(i+.5)/n]);
    const on=view.fill && pts.length>0;
    $('tvBody').classList.toggle('filled',on);
    fillCanvas.classList.toggle('off',!on);
    $('screenFoot').textContent=on?'ΕΚΤΙΜΗΣΗ ΑΠΟ AMBILIGHT':'LIVE AMBILIGHT CAPTURE';
    if(!on){fillPrev=null;fillCtx.clearRect(0,0,FW,FH);return;}
    const target=new Float32Array(FW*FH*3);
    for(let y=0;y<FH;y++)for(let x=0;x<FW;x++){
      const px=(x+.5)/FW, py=(y+.5)/FH;
      let tw=0,r=0,g=0,b=0;
      for(const [qx,qy,c] of pts){
        // screen is ~16:9, so measure distance in that shape
        const dx=(px-qx)*1.78, dy=py-qy, d2=dx*dx+dy*dy+1e-4, w=1/d2;
        tw+=w;r+=w*c[0];g+=w*c[1];b+=w*c[2];
      }
      const k=(y*FW+x)*3;target[k]=r/tw;target[k+1]=g/tw;target[k+2]=b/tw;
    }
    if(!fillPrev)fillPrev=target;
    else for(let k=0;k<target.length;k++)fillPrev[k]+=.35*(target[k]-fillPrev[k]);
    const d=fillImage.data;
    for(let i=0;i<FW*FH;i++){
      const shown=Color.toScreen([fillPrev[i*3],fillPrev[i*3+1],fillPrev[i*3+2]],params.gain)||[0,0,0];
      d[i*4]=shown[0];d[i*4+1]=shown[1];d[i*4+2]=shown[2];d[i*4+3]=255;
    }
    fillCtx.putImageData(fillImage,0,0);
  }
  // ---- v10: light spilling onto the wall around the TV ---------------------
  // Each lit edge pixel throws a soft spot of its own colour outward, away
  // from the screen, onto a low-res canvas behind the TV that CSS blurs -
  // the way real Ambilight lights the wall. Uses the colours the edge
  // pixels actually show (setPixels stores them), so it always matches.
  const glowCanvas=$('wallGlow'), glowCtx=glowCanvas.getContext('2d');
  function drawWallGlow(){
    const area=glowCanvas.parentElement.getBoundingClientRect();
    const W=Math.max(40,Math.round(area.width/6)), H=Math.max(30,Math.round(area.height/6));
    if(glowCanvas.width!==W||glowCanvas.height!==H){glowCanvas.width=W;glowCanvas.height=H;}
    glowCtx.clearRect(0,0,W,H);
    glowCanvas.style.opacity=view.glow?'':'0';
    if(!view.glow||!connected)return;
    const sx=W/area.width, sy=H/area.height;
    const out={top:[0,-1],bottom:[0,1],left:[-1,0],right:[1,0]};
    glowCtx.globalCompositeOperation='screen';  // overlapping spots must not add up to white
    for(const [side,el] of Object.entries(edges)){
      const [ox,oy]=out[side];
      for(const px of el.children){
        const c=px._shown; if(!c)continue;
        const r=px.getBoundingClientRect();
        const cx=(r.left+r.width/2-area.left)*sx, cy=(r.top+r.height/2-area.top)*sy;
        const reach=(side==='top'||side==='bottom'?area.height*sy:area.width*sx)*.1;
        const size=Math.max(r.width*sx,r.height*sy)*1.6+reach*.9;
        const gx=cx+ox*reach*.75, gy=cy+oy*reach*.75;
        const grad=glowCtx.createRadialGradient(gx,gy,0,gx,gy,size);
        grad.addColorStop(0,`rgba(${c[0]},${c[1]},${c[2]},.42)`);
        grad.addColorStop(1,`rgba(${c[0]},${c[1]},${c[2]},0)`);
        glowCtx.fillStyle=grad;
        glowCtx.fillRect(gx-size,gy-size,size*2,size*2);
      }
    }
    glowCtx.globalCompositeOperation='source-over';
  }
  function draw(){
    if(!state || !connected){
      for(const el of Object.values(edges))setPixels(el,[]);
      $('frameMark').textContent='OFFLINE';
      $('powerEstimate').textContent='—';
      $('powerMeter').style.width='0%';
      renderOutput(null);
      $('stripSummary').textContent='Ταινία LED';
      $('cornerStatus').textContent='Χωρίς ζωντανά δεδομένα';
      drawScreenFill([],[],[],[]);
      drawWallGlow();
      return;
    }
    const s=state;
    const hueLive=['live','demo'].includes(s.sources?.hue?.status);
    const jsLive=['live','demo'].includes(s.sources?.jointspace?.status);
    const nativeLeft=jsLive?(s.sides?.left||[]).map(s.demo?Color.linear:Color.jointspaceSide):[];
    const nativeRight=jsLive?(s.sides?.right||[]).map(s.demo?Color.linear:Color.jointspaceSide):[];
    let left=nativeLeft.slice().reverse();
    let right=nativeRight.slice();
    let top=jsLive?(s.sides?.top||[]).map(s.demo?Color.linear:Color.jointspaceSide):[];
    const bottom=hueLive?(s.bottom||[]).map(lamp=>hueDisplay(lamp,s)):[];
    const out=s.output||{};
    const strip=Array.isArray(out.leds)&&out.leds.every(isColor)?out.leds:[];
    const bottomLive=hueLive&&strip.length>0;
    if(view.sides==='leds'){
      left=interpolate(left,9);right=interpolate(right,9);top=interpolate(top,28);
    }
    const lampsMode=view.bottom==='lamps';
    // strip values are linear LED PWM levels: Color.toScreen() (inside
    // setPixels) takes linear 0..255, so they are drawn as the light the
    // LEDs would emit - no extra conversion.
    const inset=out.limit&&out.limit.lamp_inset!=null?out.limit.lamp_inset:(stripServerInset??0.25);
    const lim=out.limit||{};
    const blend=lim.blend_mode||stripServerModes.blend_mode||'smooth';
    const cornerMode=lim.corner_mode||stripServerModes.corner_mode||'side';
    const nLampsCfg=(s.bottom||[]).length||runningLamps();
    // JointSpace order: left[0] and right[last] touch the bottom (raw zones, before the 9/28/9 view).
    const jsL=jsLive?(s.sides?.left||[]).map(s.demo?Color.linear:Color.jointspaceSide):[];
    const jsR=jsLive?(s.sides?.right||[]).map(s.demo?Color.linear:Color.jointspaceSide):[];
    const endLamps=lim.end_lamps!=null?Boolean(lim.end_lamps):Boolean(stripServerModes.end_lamps&&nLampsCfg===5&&inset>0);
    const cL=cornerMode==='side'&&!endLamps?jsL[0]:null, cR=cornerMode==='side'&&!endLamps?jsR[jsR.length-1]:null;
    const softness=lim.blend_softness!=null?lim.blend_softness:(stripServerModes.blend_softness||0);
    // v0.30: layout + edge colour, from the live output (else the saved settings)
    const layout=lim.strip_layout||stripServerModes.strip_layout||'tv', tvLayout=layout==='tv';
    const edgeSrc=lim.edge_source||(!tvLayout?'none':(stripServerModes.edge_source==='jointspace'?'jointspace':nLampsCfg===5?'lamps':stripServerModes.edge_source==='lamps'?'none':'jointspace'));
    const edgeLeds=Number(lim.edge_leds??stripServerModes.edge_leds??3), edgeShare0=clamp(Number(lim.edge_strength??stripServerModes.edge_strength??0.5),0,1);
    const cw=clamp(Number(lim.corner_weight??1),0,1);
    const edge=!tvLayout||edgeSrc==='none'?null:edgeSrc==='lamps'?(bottom.length===5?{leds:edgeLeds,share:edgeShare0,left:bottom[0],right:bottom[4]}:null)
      :(jsL.length&&jsR.length&&cw>0?{leds:edgeLeds,share:edgeShare0*cw,left:jsL[0],right:jsR[jsR.length-1]}:null);
    if(edge)edge.ledCount=strip.length||Number(stripServerModes.led_count)||43;
    const bottom28=view.bottom!=='leds'?[]:tvLayout?edgeMix(blendAnchorsJs(tvAnchors(bottom,blend,null,false),28,blend,softness),edge):interpolateInset(bottom,28,inset,blend,cL,cR,softness,endLamps);
    const zoneAnchors=tvLayout?tvAnchors(bottom,blend,edge,true):stripAnchors(bottom,inset,blend,cL,cR,false,endLamps);
    const nb=nLampsCfg===5&&tvLayout?3:nLampsCfg;
    const nZones=bottom.length?zoneAnchors.length:(blend==='zones_mid'?2*nb-1:nb)+(tvLayout?(edgeSrc!=='none'&&edgeLeds>0&&edgeShare0>0?2:0):(cornerMode==='side'&&inset>0&&!endLamps?2:0));
    $('bottomZonesLabel').textContent=String(nZones);
    const displayedBottom=lampsMode?(bottom.length&&bottom.every(isColor)?bottom:[]):view.bottom==='leds'?bottom28:view.bottom==='zones'?zoneAnchors.map(a=>a[1].slice()):(bottomLive?strip:[]);
    const nLamps=(s.bottom||[]).length||3;
    $('stripLabel').textContent=strip.length?String(strip.length):'—';
    const blendName={smooth:'ομαλή',zones:'ζώνες',zones_mid:'ζώνες + ενδιάμεσες'}[blend]||blend;
    const edgeName={lamps:'Λ1 / Λ5',jointspace:'JointSpace',none:'—'}[edgeSrc]||edgeSrc;
    const cornerName=tvLayout?(edge&&edgeLeds>0&&edge.share>0?`χρώμα άκρης ${edgeName} · ${edgeLeds} LED · ${Math.round(edge.share*100)} %`:edgeSrc==='jointspace'&&edgeLeds>0&&edgeShare0>0?'χρώμα άκρης: αναμονή JointSpace':'χωρίς χρώμα άκρης'):endLamps?'Λ1 / Λ5 στις άκρες':cornerMode==='side'?(isColor(cL)||isColor(cR)?'μείξη με πλευρά':'λάμπα (χωρίς JointSpace)'):'λάμπα';
    const lampPosText=tvLayout?(nLampsCfg===5?'Λ2 / Λ3 / Λ4 στο 0 / 50 / 100':nLampsCfg===3?'0 / 50 / 100':Array.from({length:nb},(_,j)=>nb===1?50:Math.round(100*j/(nb-1))).join(' / ')):endLamps?`0 / ${Math.round(inset*100)} / 50 / ${Math.round((1-inset)*100)} / 100`:`${Math.round(inset*100)} / 50 / ${Math.round((1-inset)*100)}`;
    const zoneKinds=zoneAnchors.map(a=>a[2]);
    $('stripSummary').textContent=view.bottom==='zones'?`${zoneAnchors.length||'—'} ζώνες · ${zoneKinds.filter(k=>k==='corner').length} γωνιακές, ${zoneKinds.filter(k=>k==='lamp').length} λαμπών, ${zoneKinds.filter(k=>k==='mid').length} ενδιάμεσες`:lampsMode?`${nLamps} Hue λάμπες · χωρίς παρεμβολή`:view.bottom==='leds'?`28 θέσεις · όπως η πάνω πλευρά (δεν στέλνεται)`:`${strip.length||'—'} LED · ό,τι πάει στο WLED`;
    const cornerLabel=tvLayout?'άκρες':'γωνίες';
    $('cornerStatus').textContent=view.bottom==='zones'?(bottom.length?`${blendName} · ${cornerLabel}: ${cornerName} · ίσο πλάτος ανά ζώνη (δεν στέλνεται)`:'Αναμονή Hue stream'):view.bottom==='leds'?(bottom.length?`${tvLayout&&nLampsCfg===5?lampPosText:`Λάμπες στο ${lampPosText}`} % · ${blendName} · ${cornerLabel}: ${cornerName}`:'Αναμονή Hue stream'):lampsMode?(bottom.length?'Ανεξάρτητες είσοδοι · χωρίς όριο ρεύματος':'Αναμονή Hue stream'):!bottomLive?'Αναμονή Hue stream':`${blendName} · ${cornerLabel}: ${cornerName} · με όριο ρεύματος`;
    setPixels(edges.left,left);setPixels(edges.right,right);setPixels(edges.top,top);setPixels(edges.bottom,displayedBottom);
    const peakSet=new Set(!lampsMode&&view.bottom==='strip'&&bottomLive&&out.limit?out.limit.peak_leds||[]:[]);
    Array.from(edges.bottom.children).forEach((el,i)=>el.classList.toggle('peak',peakSet.has(i)));
    drawScreenFill(top,left,right,displayedBottom);
    drawWallGlow();
    $('frameMark').textContent = hueLive ? `HUESTREAM ${s.sources.hue.version||'v1'}` : 'HUE INACTIVE';
    renderOutput(out);
  }
  // v0.12: right-hand "Έξοδος ταινίας" panel, straight from state.output.
  function renderOutput(out){
    const live=Boolean(out&&out.available);
    const w=out&&out.wled;
    $('powerEstimate').textContent=live?`${Math.round(out.estimated_ma)} mA`:'—';
    $('powerBudget').textContent=out&&out.budget_ma?`${Math.round(out.budget_ma)} mA`:'—';
    $('powerMeter').style.width=live&&out.budget_ma?`${clamp(100*out.estimated_ma/out.budget_ma,0,100)}%`:'0%';
    $('outLeds').textContent=live?String(out.leds.length):'—';
    $('outPerLed').textContent=out&&out.ma_per_led_full_white?`${out.ma_per_led_full_white} mA`:'—';
    $('outAnchors').textContent=live?String(out.used_anchors):'—';
    const lim=out&&out.limit;
    const modeNames={none:'χωρίς όριο (χωράει)',anchors:'λιγότερα σημεία (pe=0)',uniform:'ομοιόμορφο χαμήλωμα (pe=0)',peaks:'κορυφές κρατιούνται'};
    const pct=v=>`${Math.round(100*(Number(v)||0))}%`;
    const phaseNames={hold:'το stream σταμάτησε · κρατά το καρέ',fade:'το stream σταμάτησε · σβήνει',black:'το stream σταμάτησε · μαύρο',stopped:'το stream σταμάτησε · δεν στέλνει (WLED preset)'};
    $('outMode').textContent=live&&lim?(lim.output_phase&&lim.output_phase!=='live'?phaseNames[lim.output_phase]||lim.output_phase:(modeNames[lim.mode]||lim.mode)):'—';
    $('outScales').textContent=live&&lim?`${pct(lim.peak_scale)} / ${pct(lim.min_scale)}`:'—';
    $('outPeaks').textContent=lim?`${lim.peak_emphasis} · ${(lim.peak_leds||[]).length||'—'}`:'—';
    $('outRise').textContent=lim?`${lim.rise_ms} ms`:'—';
    $('outWanted').textContent=live&&lim&&lim.wanted_ma?(lim.mode==='none'?`${Math.round(lim.wanted_ma)} mA (χωράει)`:`${Math.round(lim.wanted_ma)} → ${Math.round(out.estimated_ma)} mA`):'—';
    const amp=lim?`±${Number(lim.peak_drift_leds).toFixed(1).replace(/\.0$/,'')} LED${lim.peak_drift_auto?' (auto)':''}${lim.peak_drift_shape==='sine'?' · ημίτονο':''}`:'';
    $('outDrift').textContent=!lim?'—':!(lim.peak_drift_leds>0)?'σταθερές':lim.mode==='peaks'?`${amp} · τώρα ${lim.peak_offset_leds>0?'+':''}${Number(lim.peak_offset_leds).toFixed(1)}`:`${amp} (μόνο όταν δεν χωράει)`;
    $('outCompare').textContent=live&&out.compare_pe0?`${Math.round(out.compare_pe0.estimated_ma)} mA`:'—';
    $('wledTarget').textContent=!w?'—':w.enabled?w.target:'ανενεργό (χωρίς --wled-ip)';
    $('wledFps').textContent=w&&w.enabled?(Number(w.fps)||0).toFixed(1):'—';
    $('wledErrors').textContent=!w||!w.enabled?'—':w.last_error?`${w.errors} · ${w.last_error}`:String(w.errors);
    $('wledDot').classList.toggle('live',Boolean(connected&&w&&w.enabled&&w.fps>0&&!w.last_error));
  }
  // ---- bridge/service panel: identity, pairing count, systemd buttons ----
  let controlEnabled = false;
  function renderBridgePanel(s){
    if(!s || !s.identity){
      $('bridgeName').textContent='—';$('bridgeMac').textContent='—';
      $('bridgeIdField').textContent='—';$('bridgeIp').textContent='—';
      $('bridgePairings').textContent='—';
      $('linkStatus').textContent='—';$('btnPair').disabled=true;
      $('tabPairBadge').hidden=true;$('pairHero').classList.remove('open');
      $('pairBanner').hidden=true;
      controlEnabled=false;serviceCtl=false;
      for(const id of ['btnStart','btnStop','btnRestart','btnExit'])$(id).disabled=true;
      $('serviceBanner').hidden=true;
      if(!serviceBusy)$('serviceNote').textContent='Αποσυνδεδεμένο από το bridge.';
      return;
    }
    $('bridgeName').textContent=s.identity.name||'—';
    $('bridgeMac').textContent=s.identity.mac||'—';
    $('bridgeIdField').textContent=s.identity.bridgeid||'—';
    $('bridgeIp').textContent=s.identity.ip||'—';
    $('bridgePairings').textContent=String(s.pairing_known_users??'—');
    const lb=s.link_button||{};
    const left=Math.ceil((lb.remaining_ms||0)/1000);
    $('btnPair').disabled=false;
    $('tabPairBadge').hidden=!(left>0||lb.tv_waiting);
    $('pairHero').classList.toggle('open',left>0);
    renderPairBanner(lb,left);
    $('linkStatus').textContent = left>0
      ? `ΑΝΟΙΧΤΟ — ${left} s (κάνε τώρα διαμόρφωση στην TV)`
      : (lb.required ? 'Κλειστό — πάτα Pair πριν τη διαμόρφωση στην TV'
                     : 'Δεν χρειάζεται (--pairing auto)');
    controlEnabled=Boolean(s.control_enabled);
    serviceCtl=!controlEnabled&&Boolean(s.service_controls);
    serviceState=s.service||'running';
    // the page saw the new state (Stop/Start done): unlock the buttons at once
    if(serviceBusy&&serviceBusyFrom&&serviceState!==serviceBusyFrom){serviceBusy=false;serviceBusyFrom='';}
    if(s.bridge_version)$('footVersion').textContent='ambi-bridge-cpp '+s.bridge_version;
    renderServiceButtons();
  }
  // v0.15: the TV's pairing request was refused (button not pressed) ->
  // ask for Pair on both tabs; when the pairing then succeeds, say so.
  function renderPairBanner(lb,left){
    const b=$('pairBanner');
    const justPaired=lb.paired_ago_ms!=null&&lb.paired_ago_ms<15000;
    if(lb.tv_waiting&&left<=0){
      b.className='pair-banner';
      $('pairBannerTitle').textContent='Η TV ζητά σύζευξη';
      $('pairBannerText').textContent='Η TV προσπάθησε να συνδεθεί αλλά το κουμπί δεν ήταν πατημένο. Πάτα Pair εδώ (ή p στο terminal).';
      $('btnPairBanner').hidden=false;b.hidden=false;
    }else if(left>0&&!justPaired){
      b.className='pair-banner open';
      $('pairBannerTitle').textContent=`Pair ανοιχτό — ${left} s`;
      $('pairBannerText').textContent=lb.tv_waiting
        ?'Περιμένουμε την TV να ξαναδοκιμάσει. Αν δεν συνδεθεί μόνη της, πάτα ξανά «Διαμόρφωση» στο Ambilight+hue της TV.'
        :'Κάνε τώρα «Διαμόρφωση» στο Ambilight+hue της TV.';
      $('btnPairBanner').hidden=true;b.hidden=false;
    }else if(justPaired){
      b.className='pair-banner done';
      $('pairBannerTitle').textContent='Συνδέθηκε ✓';
      $('pairBannerText').textContent='Η TV ολοκλήρωσε τη σύζευξη με το bridge.';
      $('btnPairBanner').hidden=true;b.hidden=false;
    }else b.hidden=true;
  }
  // ---- v0.28: Start / Stop / Restart / Exit (in place) or systemd Stop/Restart ----
  let serviceCtl=false,serviceState='running',serviceBusy=false,serviceBusyFrom='',loginInfo=null;
  const serviceErrors={already_stopped:'είναι ήδη σταματημένη',already_running:'τρέχει ήδη',already_restarting:'γίνεται ήδη',
    binary_failed:'το νέο build δεν ξεκινά (--version απέτυχε)· το bridge συνεχίζει όπως ήταν',no_binary:'δεν βρέθηκε το αρχείο του προγράμματος',
    local_browser_required:'μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση»',not_available:'δεν διατίθεται εδώ (systemd)'};
  function renderServiceButtons(){
    const stopped=serviceState==='stopped';
    const edit=Boolean(loginInfo&&loginInfo.can_edit);
    $('serviceBanner').hidden=!stopped;
    $('btnStartBanner').disabled=!serviceCtl||!edit||serviceBusy;
    if(controlEnabled){
      $('btnStart').disabled=true;$('btnExit').disabled=true;
      $('btnStop').disabled=false;$('btnRestart').disabled=false;
      if(!serviceBusy)$('serviceNote').textContent='systemd service: Stop/Restart μέσω systemctl.';
      return;
    }
    $('btnStart').disabled=!serviceCtl||!edit||!stopped||serviceBusy;
    $('btnStop').disabled=!serviceCtl||!edit||stopped||serviceBusy;
    $('btnRestart').disabled=!serviceCtl||!edit||serviceBusy;
    $('btnExit').disabled=!serviceCtl||!edit||serviceBusy;
    if(serviceBusy)return;
    $('serviceNote').textContent=!serviceCtl?'Start/Stop δεν διατίθενται σε αυτό το bridge.'
      :!edit?'Start/Stop/Restart/Έξοδος: μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση» πάνω δεξιά.'
      :(stopped?'Η υπηρεσία είναι σταματημένη. ':'Η υπηρεσία τρέχει. ')+'Stop: σταματά την επικοινωνία με την TV και την ταινία, η σελίδα και το terminal μένουν. Restart: ξανά από το νεότερο build. Έξοδος: κλείνει το πρόγραμμα (ξεκινά ξανά μόνο από το terminal: ambi). Στο terminal: q stop, s start, r δύο φορές restart, x έξοδος.';
  }
  async function service(action){
    if(action==='exit'&&!confirm('Έξοδος από το πρόγραμμα; Η σελίδα δεν θα ξαναφορτώσει μέχρι να το ξεκινήσεις από το terminal (ambi).'))return;
    serviceBusy=true;serviceBusyFrom=action==='stop'||action==='start'?serviceState:'';renderServiceButtons();
    const what={start:'Εκκίνηση',stop:'Διακοπή',restart:'Επανεκκίνηση',exit:'Έξοδος'}[action];
    $('serviceNote').textContent=what+' σε εξέλιξη…';
    try{
      const r=await fetch('/api/service',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action})});
      const j=await r.json().catch(()=>({}));
      if(!r.ok)throw new Error(serviceErrors[j.error]||j.error||('http '+r.status));
      $('serviceNote').textContent=action==='exit'?'Το πρόγραμμα έκλεισε. Ξεκινά ξανά μόνο από το terminal (ambi).'
        :what+': η σελίδα θα επανασυνδεθεί μόνη της σε λίγα δευτερόλεπτα.'+(action==='restart'?' Αν η TV δεν ξαναπιάσει: Ambilight+hue off/on.':'');
      setTimeout(()=>{serviceBusy=false;renderServiceButtons();},action==='exit'?600000:8000);
    }catch(e){
      serviceBusy=false;renderServiceButtons();
      $('serviceNote').textContent='Δεν έγινε: '+e.message;
    }
  }
  async function control(action){
    if(!controlEnabled)return service(action);
    $('serviceNote').textContent=(action==='stop'?'Διακοπή':'Επανεκκίνηση')+' σε εξέλιξη…';
    try{
      const r=await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action})});
      if(!r.ok)throw new Error('http '+r.status);
      $('serviceNote').textContent = action==='stop'
        ? 'Διακόπηκε — η σελίδα θα μείνει χωρίς σύνδεση μέχρι να ξεκινήσει ξανά η υπηρεσία.'
        : 'Επανεκκινείται — η σελίδα θα επανασυνδεθεί μόνη της σε λίγα δευτερόλεπτα.';
    }catch(e){
      $('serviceNote').textContent='Αποτυχία αιτήματος: '+e;
    }
  }
  async function pressPair(){
    try{
      const r=await fetch('/api/pair',{method:'POST'});
      if(!r.ok)throw new Error('http '+r.status);
    }catch(e){
      $('linkStatus').textContent='Αποτυχία: '+e;
      $('pairBannerText').textContent='Αποτυχία: '+e;
    }
  }
  $('btnPair').addEventListener('click',pressPair);
  $('btnPairBanner').addEventListener('click',pressPair);
  $('btnRestart').addEventListener('click',()=>control('restart'));
  $('btnStop').addEventListener('click',()=>control('stop'));
  $('btnStart').addEventListener('click',()=>service('start'));
  $('btnStartBanner').addEventListener('click',()=>service('start'));
  $('btnExit').addEventListener('click',()=>service('exit'));
  // ---- v0.28: login (other devices) ----
  const loginErrors={username_1_to_32_chars:'Όνομα: 1–32 χαρακτήρες.',username_ascii_no_spaces:'Όνομα: λατινικοί χαρακτήρες χωρίς κενά.',
    password_min_8_chars:'Κωδικός: τουλάχιστον 8 χαρακτήρες.',password_max_128_chars:'Κωδικός: έως 128 χαρακτήρες.',
    save_failed:'Δεν γράφτηκε στο δίσκο (δικαιώματα φακέλου;).',local_browser_required:'Μόνο από τον browser του ίδιου Linux (127.0.0.1).',
    bad_credentials:'Λάθος όνομα ή κωδικός.',not_configured:'Δεν έχει οριστεί login σε αυτό το bridge.',same_origin_required:'Άνοιξε τη σελίδα από τη διεύθυνση του bridge.'};
  async function loadLogin(){
    try{
      const r=await fetch('/api/login',{cache:'no-store'});
      if(!r.ok)throw new Error('http '+r.status);
      loginInfo=await r.json();
    }catch(e){loginInfo=null;}
    const L=loginInfo||{};
    const btn=$('loginButton');
    btn.hidden=!L.available||L.local||!L.configured;
    btn.classList.toggle('in',Boolean(L.logged_in));
    btn.textContent=L.logged_in?'Αποσύνδεση ('+(L.username||'')+')':'Σύνδεση';
    $('loginDays').textContent=String(L.session_days||7);
    $('accessForm').hidden=!L.local;
    $('accClear').disabled=!L.configured;
    $('accLogoutRow').hidden=!L.logged_in;
    if(L.local&&L.configured&&!$('accUser').value)$('accUser').value=L.username||'';
    $('accessNote').textContent=!loginInfo?'Δεν διαβάστηκε η κατάσταση login.'
      :!L.available?'Δεν διατίθεται σε αυτό το bridge.'
      :L.local?(L.configured?'Login ενεργό για τον χρήστη «'+L.username+'». Νέος κωδικός εδώ = αποσυνδέονται όλες οι συσκευές.':'Δεν έχει οριστεί login: αλλαγές μόνο από εδώ.')
      :L.logged_in?'Συνδεδεμένος ως «'+L.username+'»: μπορείς να αλλάζεις ρυθμίσεις από αυτή τη συσκευή.'
      :L.configured?'Πάτα «Σύνδεση» πάνω δεξιά για αλλαγές από αυτή τη συσκευή.'
      :'Δεν έχει οριστεί login. Όρισέ το από τον browser του ίδιου Linux (http://127.0.0.1:'+location.port+'/monitor).';
    renderServiceButtons();
  }
  async function logout(){
    try{await fetch('/api/logout',{method:'POST'});}catch(e){}
    location.reload();
  }
  $('loginButton').addEventListener('click',()=>{
    if(loginInfo&&loginInfo.logged_in)return logout();
    $('loginErr').textContent='';$('loginPass').value='';
    $('loginDialog').showModal();$('loginUser').focus();
  });
  $('accLogout').addEventListener('click',logout);
  $('loginCancel').addEventListener('click',()=>$('loginDialog').close());
  $('loginForm').addEventListener('submit',async e=>{
    e.preventDefault();
    $('loginSubmit').disabled=true;$('loginErr').textContent='';
    try{
      const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({username:$('loginUser').value,password:$('loginPass').value})});
      const j=await r.json().catch(()=>({}));
      if(r.ok){location.reload();return;}
      $('loginErr').textContent=j.error==='locked'?`Πολλές λάθος προσπάθειες. Δοκίμασε ξανά σε ${Math.ceil(j.retry_s||60)} s.`:(loginErrors[j.error]||('Σφάλμα: '+(j.error||r.status)));
    }catch(err){$('loginErr').textContent='Σφάλμα σύνδεσης με το bridge.';}
    finally{$('loginSubmit').disabled=false;$('loginPass').value='';}
  });
  async function loginSetup(body){
    try{
      const r=await fetch('/api/login/setup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
      const j=await r.json().catch(()=>({}));
      if(!r.ok)throw new Error(loginErrors[j.error]||j.error||('http '+r.status));
      $('accPass').value='';$('accPass2').value='';
      await loadLogin();
      $('accessNote').textContent=(body.action==='set'?'Αποθηκεύτηκε. ':'Το login αφαιρέθηκε. ')+$('accessNote').textContent;
    }catch(e){$('accessNote').textContent='Δεν έγινε: '+e.message;}
  }
  $('accessForm').addEventListener('submit',e=>{
    e.preventDefault();
    if($('accPass').value!==$('accPass2').value){$('accessNote').textContent='Οι δύο κωδικοί διαφέρουν.';return;}
    loginSetup({action:'set',username:$('accUser').value.trim(),password:$('accPass').value});
  });
  $('accClear').addEventListener('click',()=>{
    if(confirm('Αφαίρεση login; Οι άλλες συσκευές αποσυνδέονται και οι αλλαγές γίνονται πάλι μόνο από αυτό το Linux.'))loginSetup({action:'clear'});
  });
  loadLogin();

  function render(s){
    if(!s || s.schema!=='ambilight-monitor/1')return;
    state=s; connected=true; lastEventAt=Date.now();
    $('copyButton').disabled=false;
    $('connection').classList.replace('offline','online');
    $('connectionText').textContent='Συνδεδεμένο';
    $('inputsToggle').classList.remove('offline');
    pageUpdates.push(Date.now());
    $('demoBadge').classList.toggle('hidden',!s.demo);
    const js=s.sources.jointspace,hue=s.sources.hue;
    setSource('js',js);setSource('hue',hue);
    const topo=s.topology;
    $('jsCount').textContent=(topo.left||0)+(topo.right||0)+(topo.top||0);
    $('jsBreakdown').textContent=`${topo.left} αριστ. / ${topo.top} πάνω / ${topo.right} δεξ.`;
    $('hueCount').textContent=(s.bottom||[]).filter(x=>isColor(x.rgb)).length;
    $('hueTotal').textContent=String((s.bottom||[]).length||3);
    $('lampsLabel').textContent=String((s.bottom||[]).length||3);
    if(topo.left||topo.top||topo.right)$('zonesLabel').textContent=`${topo.left}/${topo.top}/${topo.right}`;
    $('hueVersion').textContent=`HueStream ${hue.version||'—'}`;
    $('schema').textContent=s.schema;
    $('layers').textContent=topo.layers;
    $('colorspace').textContent=hue.colorspace===1?'XY + Brightness':hue.colorspace===0?'RGB':hue.colorspace==null?'—':String(hue.colorspace);
    $('badFrames').textContent=hue.bad_frames??'—';
    $('origin').textContent=s.demo?'DEMO / synthetic':'LIVE';
    $('browserAge').textContent='τώρα';
    renderBridgePanel(s);
    updateDrawer(s); draw();
  }
  function markDisconnect(){
    connected=false;
    $('connection').classList.replace('online','offline');
    $('connectionText').textContent='Αποσυνδεδεμένο';
    $('inputsToggle').classList.add('offline');
    state=null;
    $('copyButton').disabled=true;
    for(const prefix of ['js','hue'])setSource(prefix,{status:'unavailable'});
    for(const id of ['jsCount','hueCount'])$(id).textContent='0';
    for(const id of ['jsBreakdown','hueVersion','schema','layers','colorspace','badFrames','browserAge'])$(id).textContent='—';
    $('origin').textContent='OFFLINE';
    renderBridgePanel(null);
    updateDrawer(null); draw();
  }
  function syncViewControls(){
    for(const key of ['sides','bottom'])
      document.querySelectorAll(`[data-${key}]`).forEach(btn=>{
        const on=btn.dataset[key]===view[key];
        btn.classList.toggle('selected',on);btn.setAttribute('aria-pressed',String(on));
      });
    $('optScreenFill').checked=view.fill;
    $('optWallGlow').checked=view.glow;
    $('modeNote').textContent=sideNotes[view.sides]+' '+bottomNotes[view.bottom];
  }
  for(const key of ['sides','bottom'])
    document.querySelectorAll(`[data-${key}]`).forEach(b=>b.addEventListener('click',()=>{
      view[key]=b.dataset[key];saveView();syncViewControls();draw();
    }));
  $('optScreenFill').addEventListener('change',e=>{view.fill=e.target.checked;saveView();draw();});
  $('optWallGlow').addEventListener('change',e=>{view.glow=e.target.checked;saveView();draw();});
  syncViewControls();
  // ---- v10: tabs (remembered per browser; #bridge / #live links work) ----
  function setTab(tab){
    if(!['live','bridge'].includes(tab))tab='live';
    document.querySelectorAll('[role="tab"]').forEach(t=>t.setAttribute('aria-selected',String(t.dataset.tab===tab)));
    $('paneLive').hidden=tab!=='live';
    $('paneBridge').hidden=tab!=='bridge';
    try{localStorage.setItem('ambi.tab',tab);}catch(e){}
    if(location.hash!=='#'+tab)history.replaceState(null,'','#'+tab);
    if(tab==='live')draw();
    if(tab==='bridge'&&window.ambiLoadStrip)window.ambiLoadStrip(false);
    if(tab==='bridge'&&window.ambiLoadIdentity)window.ambiLoadIdentity();
    if(tab==='bridge'&&window.ambiLoadDiscovery)window.ambiLoadDiscovery();
  }
  document.querySelectorAll('[role="tab"]').forEach(t=>t.addEventListener('click',()=>setTab(t.dataset.tab)));
  window.addEventListener('hashchange',()=>setTab(location.hash.slice(1)));
  {
    let initial=location.hash.slice(1);
    if(!initial){try{initial=localStorage.getItem('ambi.tab')||'';}catch(e){}}
    setTab(initial||'live');
  }
  function setDisplayGain(value){
    params.gain=clamp(Number(value)||1,.5,4);
    $('displayGain').value=String(params.gain);
    $('displayGainValue').textContent=`${params.gain}×`;
    updateDrawer(state);draw();
  }
  $('displayGain').addEventListener('input',e=>setDisplayGain(e.target.value));
  $('resetDisplayGain').addEventListener('click',()=>setDisplayGain(1));
  $('copyButton').addEventListener('click',async()=>{
    if(!connected || !state)return;
    const diagnostics={...state,preview:{view:{...view},display_gain:params.gain,color_model:Color.profile}};
    try{await navigator.clipboard.writeText(JSON.stringify(diagnostics,null,2));$('copyButton').textContent='Αντιγράφηκε';setTimeout(()=>$('copyButton').textContent='Αντιγραφή diagnostics',1400);}
    catch{ $('copyButton').textContent='Δεν επιτρέπεται αντιγραφή';setTimeout(()=>$('copyButton').textContent='Αντιγραφή diagnostics',1700); }
  });
  async function loadSettings(){
    const note=$('credentialsNote'),button=$('saveCredentials');
    button.disabled=true;
    try{
      const response=await fetch('/api/settings/jointspace',{cache:'no-store'});
      if(!response.ok)throw new Error('settings');
      const settings=await response.json();
      button.disabled=!settings.can_edit;
      $('tvUser').disabled=!settings.can_edit;
      $('tvPass').disabled=!settings.can_edit;
      note.textContent=!settings.available?'Διαθέσιμο όταν ορίσεις --tv-ip στην εκκίνηση της bridge.':
        !settings.can_edit?'Μόνο προβολή. Για αλλαγή: «Σύνδεση» πάνω δεξιά (αν έχει οριστεί login), ή ο browser του ίδιου Linux ως http://127.0.0.1:'+location.port+'/monitor.':
        settings.load_error?'Το αποθηκευμένο αρχείο δεν διαβάστηκε. Συμπλήρωσε ξανά τα credentials.':
        settings.persisted?'Υπάρχουν αποθηκευμένα credentials. Συμπλήρωσε μόνο για αλλαγή τους.':
        settings.configured?'Υπάρχουν credentials στη συνεδρία. Μπορείς να τα αποθηκεύσεις εδώ.':
        'Συμπλήρωσε τα JointSpace pairing credentials της TV.';
    }catch{
      note.textContent='Οι ρυθμίσεις δεν είναι διαθέσιμες. Έλεγξε τη σύνδεση με τη live bridge.';
    }
  }
  $('credentialsForm').addEventListener('submit',async e=>{
    e.preventDefault();
    const button=$('saveCredentials'),note=$('credentialsNote');
    button.disabled=true;
    try{
      const response=await fetch('/api/settings/jointspace',{
        method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({username:$('tvUser').value,password:$('tvPass').value})
      });
      if(!response.ok)throw new Error('save');
      $('tvUser').value='';$('tvPass').value='';
      note.textContent='Αποθηκεύτηκαν στο Linux. Η κατάσταση JointSpace δείχνει αν η σύνδεση πέτυχε.';
    }catch{
      note.textContent='Δεν αποθηκεύτηκαν. Άνοιξε από localhost και έλεγξε τις τιμές και τα δικαιώματα του φακέλου.';
    }finally{button.disabled=false;}
  });
  $('credentialsPanel').addEventListener('toggle',()=>{if($('credentialsPanel').open)loadSettings();});
  loadSettings();
  // ---- v0.15 (Phase 3): strip & current settings ----
  const stripFields={ma_per_led_full_white:'setMaPerLed',current_budget_ma:'setBudget',led_count:'setLedCount',
    idle_ma_per_led:'setIdle',peak_emphasis:'setPeak',rise_ms:'setRise',fade_in_ms:'setFadeIn',fade_out_ms:'setFadeOut',smooth_ms:'setSmooth',wb_r:'setWbR',wb_g:'setWbG',wb_b:'setWbB',wb_keep_pure:'setWbKeep',lamp_inset:'setInset',
    blend_mode:'setBlend',corner_mode:'setCorner',blend_softness:'setSoftness',
    peak_drift_leds:'setDrift',peak_drift_period_s:'setDriftPeriod',peak_drift_shape:'setDriftShape',peak_points:'setPeakPoints',
    end_lamps:'setEndLamps',strip_layout:'setLayout',edge_source:'setEdgeSource',edge_leds:'setEdgeLeds',edge_strength:'setEdgeStrength',
    loss_hold_ms:'setLossHold',loss_fade_ms:'setLossFade',loss_black_s:'setLossBlack'};
  const stripStringFields=new Set(['blend_mode','corner_mode','peak_drift_shape','strip_layout','edge_source']);
  // v0.24: end_lamps is a bool (select "true"/"false"), peak_drift_leds a number or "auto"
  const sameVal=(a,b)=>String(a)===String(b)||(a!==''&&b!==''&&Number(a)===Number(b));
  let stripServer=null,stripDefaults=null,stripCanEdit=false;
  function fillStrip(v){for(const [k,id] of Object.entries(stripFields))if(v&&v[k]!=null)$(id).value=String(v[k]);updateStripSummary();}
  function readStrip(){
    const out={};
    for(const [k,id] of Object.entries(stripFields)){
      if(stripStringFields.has(k)){if($(id).value)out[k]=$(id).value;continue;}
      if(k==='end_lamps'||k==='wb_keep_pure'){if($(id).value)out[k]=$(id).value==='true';continue;}
      const raw=$(id).value.trim().replace(',','.');
      if(raw==='')continue;
      if(k==='peak_drift_leds'&&raw.toLowerCase()==='auto'){out[k]='auto';continue;}
      const n=Number(raw);
      if(Number.isFinite(n))out[k]=k==='led_count'||k==='peak_points'||k==='edge_leds'?Math.round(n):n;
    }
    return out;
  }
  function stripDirty(){
    if(!stripServer)return false;
    const v=readStrip();
    return Object.keys(stripFields).some(k=>v[k]!==undefined&&!sameVal(v[k],stripServer[k]));
  }
  function updateStripSummary(){
    const v={...(stripServer||{}),...readStrip()};
    const n=v.led_count,idle=v.idle_ma_per_led,budget=v.current_budget_ma;
    // v0.33: white = the three channels together (ma_scale_r/g/b, 1/1/1 unless measured)
    const chan=((v.ma_scale_r??1)+(v.ma_scale_g??1)+(v.ma_scale_b??1))/3;
    const per=Math.round(v.ma_per_led_full_white*chan*10)/10;
    if(![n,per,idle,budget].every(Number.isFinite)){$('stripFormSummary').textContent='—';return;}
    const full=n*(per+idle);
    const pct=full>0?Math.min(100,Math.max(0,(budget-n*idle)/(n*per)*100)):100;
    $('stripFormSummary').innerHTML=`Όλη η ταινία σε πλήρες λευκό θα ήθελε <b>${Math.round(full)} mA</b> (${n} × ${per} + ${n} × ${idle} σβηστά). `+
      (budget>=full?`Το όριο <b>${Math.round(budget)} mA</b> φτάνει για όλα, το bridge δεν θα χρειαστεί να χαμηλώσει τίποτα.`
                   :`Με όριο <b>${Math.round(budget)} mA</b> αυτό χωράει περίπου στο <b>${pct.toFixed(0)} %</b>, άρα το bridge θα χαμηλώσει την ταινία όταν η εικόνα είναι πολύ φωτεινή.`);
    const measured=stripDefaults&&Number(v.ma_per_led_full_white)!==Number(stripDefaults.ma_per_led_full_white);
    $('hintMaPerLed').classList.toggle('warn',!measured);
    $('hintMaPerLed').textContent=measured?'Δική σου τιμή (διαφέρει από το 45 της ταινίας αναφοράς).'
      :'Το 45 μετρήθηκε στην ταινία αναφοράς (BTF WS2812B ECO 30 LED/m, ~44 mA). Άλλη ταινία: μέτρησέ την (README: «Measuring mA per LED»).';
    const inset=Number(v.lamp_inset);
    if(Number.isFinite(inset)&&inset>=0&&inset<=0.45){
      const nL=runningLamps();
      const ends=String(v.end_lamps)==='true'&&nL===5&&inset>0;
      const ps=ends?[0,Math.round(inset*1000)/10,50,Math.round((1-inset)*1000)/10,100]:nL<=1?[50]:Array.from({length:nL},(_,j)=>Math.round((inset+(1-2*inset)*j/(nL-1))*1000)/10);
      $('hintInset').textContent=`Λάμπες στο ${ps.join(' / ')} % της ταινίας. 0.25 = στα τέταρτα, 0 = στις άκρες. Πιο έξω από τις ακριανές κρατιέται το χρώμα τους.`;
    }
    $('fieldSoftness').hidden=v.blend_mode==='smooth';
    {
      const nL=runningLamps();
      const endsOn=String(v.end_lamps)==='true';
      const ends=endsOn&&nL===5&&Number(v.lamp_inset)>0;
      $('fieldCorner').classList.toggle('muted',ends);
      $('hintCorner').textContent=ends?'Δεν χρησιμοποιείται τώρα: με «Λάμπες στις άκρες» οι γωνίες είναι οι λάμπες Λ1 και Λ5 της TV. Μένει για 3 λάμπες και για σύγκριση.'
        :'Μείξη: η άκρη παίρνει το ενδιάμεσο της ακριανής λάμπας και του κάτω χρώματος της αριστερής/δεξιάς πλευράς (JointSpace). Χωρίς JointSpace κρατά τη λάμπα. Θέλει «Θέση ακριανών λαμπών» πάνω από 0.';
      $('hintEndLamps').classList.toggle('warn',endsOn&&!ends);
      $('hintEndLamps').textContent=endsOn&&!ends?(nL!==5?'Ενεργό, αλλά η ταυτότητα έχει 3 λάμπες: δεν κάνει τίποτα (χρειάζονται 5).':'Ενεργό, αλλά «Θέση ακριανών λαμπών» = 0: δεν κάνει τίποτα.')
        :'Με 5 λάμπες: η Λ1 και η Λ5 (οι γωνίες της TV) πάνε στις άκρες της ταινίας, οι Λ2–Λ4 στις θέσεις των 3 λαμπών. Τότε δεν μπαίνουν γωνίες από το JointSpace και οι κορυφές του ορίου είναι μόνο στις Λ2–Λ4. Με 3 λάμπες δεν κάνει τίποτα.';
    }
    {
      // v0.30: the tv layout ignores inset / end lamps / corners; the inset layout ignores the edge colour
      const tv=(v.strip_layout||'tv')==='tv', nL=runningLamps();
      for(const id of ['fieldInset','fieldEndLamps'])$(id).classList.toggle('muted',tv);
      if(tv)$('fieldCorner').classList.add('muted');   // else: set above (muted when end lamps are on)
      for(const id of ['fieldEdgeSource','fieldEdgeLeds','fieldEdgeStrength'])$(id).classList.toggle('muted',!tv);
      if(tv){
        $('hintCorner').textContent='Δεν χρησιμοποιείται στη διάταξη «Όπως τις βλέπει η TV» (εκεί μετράει το «Χρώμα άκρης»).';
        $('hintEndLamps').classList.remove('warn');
        $('hintEndLamps').textContent='Δεν χρησιμοποιείται στη διάταξη «Όπως τις βλέπει η TV».';
        $('hintInset').textContent='Δεν χρησιμοποιείται στη διάταξη «Όπως τις βλέπει η TV»: εκεί οι κάτω λάμπες πάνε από άκρη σε άκρη.';
      }
      const src=v.edge_source||'auto';
      const res=!tv?'none':src==='jointspace'?'jointspace':nL===5?'lamps':src==='lamps'?'none':'jointspace';
      const hs=$('hintEdgeSource');
      hs.classList.toggle('warn',tv&&src==='lamps'&&nL!==5);
      hs.textContent=!tv?'Μόνο για τη διάταξη «Όπως τις βλέπει η TV».'
        :res==='lamps'?`Τώρα: λάμπες Λ1 / Λ5 (${nL} λάμπες). Η TV τις δίνει στο κάτω μέρος των πλευρών.`
        :res==='jointspace'?`Τώρα: JointSpace, η κάτω ζώνη αριστερής / δεξιάς πλευράς${nL===5?' (ζητήθηκε ρητά)':' (3 λάμπες)'}. Χωρίς JointSpace δεν μπαίνει χρώμα άκρης.`
        :'Λ1 / Λ5 ζητήθηκαν, αλλά η ταυτότητα έχει 3 λάμπες: δεν μπαίνει χρώμα άκρης.';
      $('setEdgeStrengthValue').textContent=Number.isFinite(Number(v.edge_strength))?Number(v.edge_strength).toFixed(2):'';
      $('hintPeakPoints').textContent=tv?'0 = μία κορυφή ανά κάτω λάμπα, σε ίσες αποστάσεις πάνω σε όλη την ταινία (σαν δαχτυλίδι: όταν κυλούν, όσες βγαίνουν από τη μια άκρη μπαίνουν από την άλλη). 2–8 = τόσες κορυφές. Δρα μόνο όταν το καρέ δεν χωράει στο όριο.'
        :'0 = οι κορυφές του ορίου είναι στις λάμπες, όπως πριν. 2–8 = τόσες κορυφές σε ίσες αποστάσεις πάνω στην ταινία, όχι στις λάμπες. Με τις κυλιόμενες κορυφές κινούνται όλες μαζί. Με χαμηλή «Έμφαση κορυφών» (0.2–0.3) γίνονται λίγο πιο φωτεινά σημεία που κινούνται αργά. Δρα μόνο όταν το καρέ δεν χωράει στο όριο. Δεν έχει δοκιμαστεί με το μάτι στην ταινία.';
      $('hintDrift').textContent=tv?'Όταν το καρέ δεν χωράει στο όριο, οι κορυφές κυλούν ±τόσα LED, για να μην καταπονούνται πάντα τα ίδια LED. auto = μισή απόσταση δύο κορυφών (μαζί περνούν από όλα τα LED). 0 = σταθερές κορυφές.'
        :'Όταν το καρέ δεν χωράει στο όριο, οι κορυφές πηγαινοέρχονται ±τόσα LED γύρω από τις λάμπες, για να μην καταπονούνται πάντα τα ίδια LED. auto = η απόσταση δύο κορυφών (περνούν από όλα τα LED). 0 = σταθερές κορυφές. Δεν έχει δοκιμαστεί με το μάτι στην ταινία.';
    }
    $('setSoftnessValue').textContent=Number.isFinite(Number(v.blend_softness))?Number(v.blend_softness).toFixed(2):'';
    $('resetStripForm').disabled=!stripCanEdit||!stripDirty();
  }
  function setStripEditable(on){
    for(const id of Object.values(stripFields))$(id).disabled=!on;
    $('saveStrip').disabled=!on;$('defaultsStrip').disabled=!on;
    $('resetStripForm').disabled=!on||!stripDirty();
  }
  async function loadStrip(force){
    if(!force&&stripDirty())return;  // never overwrite what the user is typing
    try{
      const r=await fetch('/api/settings/strip',{cache:'no-store'});
      if(!r.ok)throw new Error('http '+r.status);
      const j=await r.json();
      if(!j.available){$('stripNote').textContent='Δεν είναι διαθέσιμο σε αυτό το bridge.';setStripEditable(false);return;}
      stripServer=j.settings;stripDefaults=j.defaults;stripCanEdit=Boolean(j.can_edit);Color.setJointspace(stripServer.corner_js_curve,stripServer.corner_js_full_scale);if(j.limits&&j.limits.current_budget_ma_max)$('setBudget').max=j.limits.current_budget_ma_max;stripServerInset=stripServer.lamp_inset;stripServerModes=stripServer;
      fillStrip(stripServer);setStripEditable(stripCanEdit);
      $('stripNote').textContent=stripCanEdit?'Μπορείς να αλλάξεις τις τιμές από εδώ.'
        :'Μόνο προβολή. Για αλλαγή: «Σύνδεση» πάνω δεξιά (αν έχει οριστεί login), ή ο browser του ίδιου Linux ως http://127.0.0.1:'+location.port+'/monitor.';
    }catch(e){
      $('stripNote').textContent='Οι ρυθμίσεις δεν είναι διαθέσιμες. Έλεγξε τη σύνδεση με τη live bridge.';
      setStripEditable(false);
    }
  }
  for(const id of Object.values(stripFields)){$(id).addEventListener('input',updateStripSummary);$(id).addEventListener('change',updateStripSummary);}
  $('resetStripForm').addEventListener('click',()=>{fillStrip(stripServer);});
  $('defaultsStrip').addEventListener('click',()=>{fillStrip(stripDefaults);$('stripNote').textContent='Οι προεπιλογές μπήκαν στη φόρμα. Πάτα «Αποθήκευση» για να εφαρμοστούν.';});
  $('stripForm').addEventListener('submit',async e=>{
    e.preventDefault();
    if(!stripCanEdit)return;
    const button=$('saveStrip');button.disabled=true;
    const sent=readStrip();
    try{
      const r=await fetch('/api/settings/strip',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(sent)});
      if(!r.ok)throw new Error(r.status===403?'local':'http '+r.status);
      const j=await r.json();
      stripServer=j.settings;fillStrip(stripServer);stripServerInset=stripServer.lamp_inset;stripServerModes=stripServer;
      const clamped=Object.keys(sent).filter(k=>!sameVal(sent[k],stripServer[k]));
      $('stripNote').textContent=(j.saved_to_disk?'Εφαρμόστηκαν και αποθηκεύτηκαν.':'Εφαρμόστηκαν, αλλά ΔΕΝ γράφτηκαν στο δίσκο (δικαιώματα φακέλου;).')+
        (clamped.length?' Διορθώθηκαν στα όρια: '+clamped.map(k=>`${k} → ${stripServer[k]}`).join(', ')+'.':'');
    }catch(err){
      $('stripNote').textContent=String(err.message)==='local'?'Απορρίφθηκε: αλλαγές μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση» (έληξε η σύνδεση;).':'Δεν αποθηκεύτηκαν: '+err.message;
    }finally{button.disabled=!stripCanEdit;updateStripSummary();}
  });
  window.ambiLoadStrip=loadStrip;
  // ---- v0.17: identity (name, MAC, lamps) ----
  let idData=null,idSelected=null;
  // v0.27: lamp count of the RUNNING identity (known without a Hue stream); the stream's lamp count until it loads
  function runningLamps(){let n=0;try{n=idData&&idData.running&&idData.running.lamps;}catch(e){}return n||(state&&state.bottom&&state.bottom.length)||3;}
  const idErrors={name_empty:'Το όνομα είναι κενό.',name_too_long:'Έως 32 χαρακτήρες.',name_spaces_at_ends:'Χωρίς κενά στην αρχή/τέλος.',
    name_not_ascii:'Μόνο λατινικοί χαρακτήρες (η TV διαβάζει το όνομα byte-byte).',name_bad_character:'Χωρίς < > & " \'.',
    local_browser_required:'Αλλαγές μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση».',save_failed:'Δεν γράφτηκε στο δίσκο (δικαιώματα φακέλου;).',
    too_many_identities:'Έως 20 ταυτότητες.',lamps_must_be_3_or_5:'Λάμπες: 3 ή 5.'};
  function renderIdentity(){
    const list=$('identityList');list.textContent='';
    if(!idData||!idData.available){$('identityNote').textContent='Δεν είναι διαθέσιμο.';return;}
    if(!idData.identities.some(e=>e.mac===idSelected))idSelected=idData.active;
    for(const e of idData.identities){
      const b=document.createElement('button');b.type='button';b.className='id-item';b.setAttribute('role','radio');
      b.setAttribute('aria-checked',String(e.mac===idSelected));
      const main=document.createElement('div');main.className='id-main';
      const nm=document.createElement('b');nm.textContent=e.name;
      const mc=document.createElement('span');mc.textContent=`${e.mac} · ${e.lamps} λάμπες · σύζευξη: ${e.paired}`;
      main.append(nm,mc);
      const badges=document.createElement('div');badges.className='id-badges';
      const badge=(txt,cls)=>{const s=document.createElement('span');s.className='id-badge '+cls;s.textContent=txt;badges.append(s);};
      if(e.running)badge('ΤΡΕΧΕΙ','run');
      if(e.active&&!e.running)badge('ΣΤΗΝ ΕΠΟΜΕΝΗ ΕΚΚΙΝΗΣΗ','sel');
      if(e.default)badge('ΑΡΧΙΚΗ','');
      b.append(main,badges);
      b.addEventListener('click',()=>{idSelected=e.mac;renderIdentity();if(idData.can_edit&&e.mac!==idData.active)idAction({action:'select',mac:e.mac});});
      list.append(b);
    }
    const sel=idData.identities.find(e=>e.mac===idSelected);
    if(sel&&document.activeElement!==$('idName'))$('idName').value=sel.name;
    if(sel)$('idLamps').value=String(sel.lamps);
    for(const id of ['idName','idRename','idLamps','idLampsSave','idCreate'])$(id).disabled=!idData.can_edit;
    const r=$('identityRestart');
    if(idData.restart_needed){
      const a=idData.identities.find(e=>e.active)||{};
      r.innerHTML='';r.append(`Αποθηκεύτηκε. Τρέχει ακόμα «${idData.running.name}» (${idData.running.mac}, ${idData.running.lamps} λάμπες). Η αλλαγή σε «${a.name}» (${a.mac}, ${a.lamps} λάμπες) ισχύει μετά την επανεκκίνηση: `);
      const c=document.createElement('code');c.textContent='ambi off ; ambi';r.append(c);
      if(a.mac!==idData.running.mac)r.append('. Μετά: Pair εδώ και «Διαμόρφωση» στο Ambilight+hue της TV.');
      r.hidden=false;
    }else r.hidden=true;
    if(!idData.can_edit)$('identityNote').textContent='Μόνο προβολή. Για αλλαγή: «Σύνδεση» πάνω δεξιά (αν έχει οριστεί login), ή ο browser του ίδιου Linux ως http://127.0.0.1:'+location.port+'/monitor.';
  }
  async function loadIdentity(){
    try{const r=await fetch('/api/identity',{cache:'no-store'});if(!r.ok)throw new Error();idData=await r.json();if(window.ambiLoadStrip)updateStripSummary();
      if(idData.can_edit)$('identityNote').textContent='Διάλεξε ταυτότητα, άλλαξε όνομα ή λάμπες. Αποθηκεύονται αμέσως, ισχύουν στην επόμενη εκκίνηση.';
      renderIdentity();
    }catch{$('identityNote').textContent='Η ταυτότητα δεν είναι διαθέσιμη. Έλεγξε τη σύνδεση με τη live bridge.';}
  }
  async function idAction(body){
    try{
      const r=await fetch('/api/identity',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
      const j=await r.json().catch(()=>({}));
      if(!r.ok){$('identityNote').textContent='Δεν έγινε: '+(idErrors[j.error]||j.error||('http '+r.status));return;}
      idData=j;if(body.action==='create')idSelected=j.active;
      $('identityNote').textContent=body.action==='create'?'Νέα ταυτότητα με νέα MAC, επιλεγμένη για την επόμενη εκκίνηση.':'Αποθηκεύτηκε.';
      renderIdentity();
    }catch(e){$('identityNote').textContent='Δεν έγινε: '+e;}
  }
  $('identityForm').addEventListener('submit',e=>{e.preventDefault();if(idSelected)idAction({action:'rename',mac:idSelected,name:$('idName').value.trim()});});
  $('idLampsSave').addEventListener('click',()=>{if(idSelected)idAction({action:'lamps',mac:idSelected,lamps:Number($('idLamps').value)});});
  $('idCreate').addEventListener('click',()=>idAction({action:'create'}));
  window.ambiLoadIdentity=loadIdentity;
  loadIdentity();
  // ---- v0.23 (NEXT 8): the page's own connection, inside the same chip ----
  {
    const tick=()=>{
      const now=Date.now();
      while(pageUpdates.length&&now-pageUpdates[0]>5000)pageUpdates.shift();
      const age=lastEventAt?now-lastEventAt:null;
      $('pageAge').textContent=age==null?'—':age<1000?`${Math.round(age)} ms`:`${(age/1000).toFixed(1)} s`;
      $('pageRate').textContent=connected?`${(pageUpdates.length/5).toFixed(1)}/s`:'—';
      $('pageStatus').textContent=connected?'Συνδεδεμένο':'Αποσυνδεδεμένο';
      $('pageDot').classList.toggle('live',Boolean(connected));
    };
    setInterval(tick,500);tick();
  }
  // ---- v0.23 (NEXT 6): per-LED log + the limiter's compromise episodes ----
  {
    let logData=null,logScope='since_reset';
    const fmtDur=s=>{s=Number(s)||0;if(s<60)return `${Math.round(s)} s`;if(s<3600)return `${Math.round(s/60)} λεπ.`;const h=Math.floor(s/3600),m=Math.round((s-h*3600)/60);return `${h} ώρ.${m?` ${m} λεπ.`:''}`;};
    const stops=[[0x24,0x45,0x6e],[0x3f,0x9f,0xb0],[0xe7,0xb0,0x5c],[0xe8,0x60,0x4f]];
    const heat=v=>{v=clamp(v,0,1)*(stops.length-1);const i=Math.min(stops.length-2,Math.floor(v)),f=v-i;return `rgb(${stops[i].map((c,k)=>Math.round(c+(stops[i+1][k]-c)*f)).join(',')})`;};
    const ledList=a=>(a||[]).map(i=>i+1).join(' / ')||'—';
    function renderLog(){
      const d=logData;
      if(!d||!d.available){$('logTotals').textContent=d?'Δεν είναι διαθέσιμο σε αυτό το bridge.':'Αναμονή δεδομένων…';return;}
      const tot=d[logScope]||{},mah=tot.mah||[],n=mah.length,mx=Math.max(1e-9,...mah);
      const order=mah.map((v,i)=>[v,i]).sort((a,b)=>b[0]-a[0]).slice(0,5).filter(x=>x[0]>0).map(x=>x[1]);
      const bar=$('wearBar');
      if(bar.children.length!==n){bar.textContent='';for(let i=0;i<n;i++)bar.append(document.createElement('i'));}
      [...bar.children].forEach((el,i)=>{
        el.style.background=mah[i]>0?heat(mah[i]/mx):'';
        el.classList.toggle('top',order.includes(i));
        el.title=`LED ${i+1}: ${(mah[i]||0).toFixed(3)} mAh · ≥95 %: ${fmtDur(tot.s_hot?.[i])} · κορυφή: ${fmtDur(tot.s_peak?.[i])} · μέση στάθμη ${Math.round(100*(tot.avg_level?.[i]||0))} %`;
      });
      $('wearLast').textContent=n?`LED ${n}`:'—';
      const share=tot.stream_s>0?Math.round(100*tot.over_s/tot.stream_s):0;
      $('logTotals').innerHTML=!tot.frames?`Δεν έχει έρθει ακόμα καρέ${tot.since?` από ${tot.since}`:''}.`:
        `Από <b>${tot.since||'—'}</b>: ροή <b>${fmtDur(tot.stream_s)}</b>, πάνω από το όριο <b>${fmtDur(tot.over_s)}</b> (${share} %, ${tot.frames_limited} καρέ). `+
        `Το πιο φωτεινό καρέ ζήτησε <b>${Math.round(tot.max_wanted_ma)} mA</b>.`;
      $('logTop').innerHTML=order.length?'Πιο φορτωμένα: '+order.map(i=>`<span>LED <b>${i+1}</b> · ${mah[i].toFixed(3)} mAh · κορυφή ${fmtDur(tot.s_peak?.[i])}</span>`).join(''):'';
      const eps=d.episodes||[];
      $('logEpCount').textContent=String(eps.length);
      $('logEpList').innerHTML=eps.length?eps.map(e=>{
        const t=(e.start||'').slice(11)||e.start,day=(e.start||'').slice(0,10);
        const peaks=e.mode==='peaks'?`κορυφές LED ${ledList(e.peak_leds)} στο <b>${Math.round(100*e.peak_scale)} %</b>, τα υπόλοιπα έως <b>${Math.round(100*e.min_scale)} %</b>`
          :e.mode==='uniform'?`όλη η ταινία ομοιόμορφα στο <b>${Math.round(100*e.min_scale)} %</b>`:`λιγότερα σημεία (pe=0)`;
        const drift=e.mode==='peaks'&&(e.drift_max-e.drift_min)>0.05?` · κορυφές κινήθηκαν ${e.drift_min.toFixed(1)}…${e.drift_max>0?'+':''}${e.drift_max.toFixed(1)} LED`:'';
        return `<li><b>${t}</b> <span class="faint">${day}</span>${e.open?'<span class="now">ΤΩΡΑ</span>':''} · ${fmtDur(e.duration_s)} · <b>${Math.round(e.wanted_ma)} → ${Math.round(e.sent_ma)} mA</b>`+
          `<span class="ep-sub">${peaks}${drift} · χειρότερο ${(e.worst_at||'').slice(11)}</span></li>`;}).join(''):'<li>Κανένα ακόμα: όλα τα καρέ χωρούσαν στο όριο.</li>';
      $('logDaysBody').innerHTML=(d.days||[]).slice().reverse().map(x=>`<tr><td>${x.date}</td><td>${fmtDur(x.stream_s)}</td><td>${fmtDur(x.over_s)}</td><td>${x.episodes}</td><td>${(x.mah_total||0).toFixed(1)}</td></tr>`).join('')||'<tr><td colspan="5">—</td></tr>';
      $('logReset').disabled=!d.can_edit||logScope!=='since_reset';
      // v0.41 (queue item 5): wear counts from the moment the strip was mounted
      $('logInstall').disabled=!d.can_edit;
      $('logInstallNote').textContent=d.installed?`Τοποθέτηση ταινίας: ${d.since_install&&d.since_install.since||'—'}. Η καταπόνηση (ρύθμιση «wear_strength») μετρά μόνο από τότε.`
        :'Δεν έχει δηλωθεί πότε μπήκε η ταινία: η καταπόνηση δεν επηρεάζει τίποτα (ακόμα κι αν το «wear_strength» είναι πάνω από 0). Πάτα «Η ταινία τοποθετήθηκε» όταν τη βάλεις στη θέση της.';
      $('logNote').textContent=d.can_edit?'':'Μηδενισμός μόνο από τον browser του ίδιου Linux ή μετά από «Σύνδεση».';
    }
    async function loadLog(){
      try{const r=await fetch('/api/strip_stats',{cache:'no-store'});if(!r.ok)throw new Error('http '+r.status);logData=await r.json();}
      catch(e){logData=null;}
      renderLog();
    }
    document.querySelectorAll('[data-logscope]').forEach(b=>b.addEventListener('click',()=>{
      logScope=b.dataset.logscope;
      document.querySelectorAll('[data-logscope]').forEach(x=>{const on=x===b;x.classList.toggle('selected',on);x.setAttribute('aria-pressed',String(on));});
      renderLog();
    }));
    $('logReset').addEventListener('click',async()=>{
      if(!confirm('Μηδενισμός των συνόλων «από το reset»; Τα «από πάντα», οι 30 μέρες και τα επεισόδια μένουν.'))return;
      $('logReset').disabled=true;
      try{const r=await fetch('/api/strip_stats/reset',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});
        const j=await r.json();if(!r.ok){$('logNote').textContent='Δεν έγινε: '+(j.error==='local_browser_required'?'μόνο από τον browser αυτού του Linux ή μετά από «Σύνδεση»':(j.error||('http '+r.status)));}else{logData=j;}}
      catch(e){$('logNote').textContent='Δεν έγινε: '+e.message;}
      renderLog();
    });
    $('logInstall').addEventListener('click',async()=>{
      if(!confirm('Η ταινία τοποθετήθηκε (ή αλλάχτηκε) τώρα; Τα σύνολα «από την τοποθέτηση» ξεκινούν από το μηδέν και η καταπόνηση μετρά μόνο από εδώ και πέρα. Τα «από πάντα» μένουν.'))return;
      $('logInstall').disabled=true;
      try{const r=await fetch('/api/strip_stats/install',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});
        const j=await r.json();if(!r.ok){$('logNote').textContent='Δεν έγινε: '+(j.error==='local_browser_required'?'μόνο από τον browser αυτού του Linux ή μετά από «Σύνδεση»':(j.error||('http '+r.status)));}else{logData=j;}}
      catch(e){$('logNote').textContent='Δεν έγινε: '+e.message;}
      renderLog();
    });
    setInterval(loadLog,5000);loadLog();
  }
  // ---- v0.19 (NEXT 1): input status popover in the top bar ----
  {
    const box=$('inputsStatus'),btn=$('inputsToggle');
    const setOpen=open=>{box.classList.toggle('open',open);box.classList.remove('closed');btn.setAttribute('aria-expanded',String(open));};
    btn.addEventListener('click',e=>{e.stopPropagation();const open=!box.classList.contains('open');setOpen(open);if(!open){box.classList.add('closed');btn.blur();}});
    // v0.37 (user: the hover popover vanished too fast): opens on click only (no hover) and stays
    // open until the chip, ✕, Escape or a click outside it closes it.
    $('inputsClose').addEventListener('click',e=>{e.stopPropagation();setOpen(false);btn.focus();});
    document.addEventListener('click',e=>{if(!box.contains(e.target)&&box.classList.contains('open'))setOpen(false);});
    document.addEventListener('keydown',e=>{if(e.key==='Escape'&&(box.classList.contains('open')||box.contains(document.activeElement))){setOpen(false);box.classList.add('closed');btn.blur();}});
    box.addEventListener('mouseleave',()=>box.classList.remove('closed'));
  }
  // ---- v0.19: who asks + "answer only Hue searches" ----
  let discData=null;
  function guessProgram(ua,what){
    const u=ua||'';
    if(/DAFUPnP/i.test(u))return 'Windows: αναζήτηση συσκευών του ίδιου του λειτουργικού (Device Association Framework · Δίκτυο / «Συσκευές»)';
    if(/Edg\//.test(u))return 'Microsoft Edge';
    if(/Electron\//.test(u)){const m=u.match(/\)\s+([A-Za-z][\w .-]*?)\/[\d.]+\s+Chrome/);return 'Εφαρμογή Electron'+(m?` (${m[1]})`:'');}
    {const m=u.match(/Chrome\/[\d.]+\s+([A-Za-z][\w-]*)\/[\d.]+\s+Safari/);if(m&&m[1]!=='Mobile')return `Εφαρμογή ${m[1]} (ενσωματωμένος Chromium)`;}
    if(/OPR\//.test(u))return 'Opera';
    if(/Brave/.test(u))return 'Brave';
    if(/HomeAssistant|aiohttp|aiohue/i.test(u))return 'Home Assistant / Python';
    if(/Chrome\//.test(u))return /dial/i.test(what||'')?'Chrome (ή Brave/Opera), αναζήτηση cast':'Chrome (ή Brave/Opera)';
    if(/Firefox\//.test(u))return 'Firefox';
    if(/Philips|Android|TPM|Linux.*TV/i.test(u))return 'TV / Android';
    if(/Windows/i.test(u))return 'Πρόγραμμα των Windows';
    return '';
  }
  function agoText(ms){const s=Math.round(ms/1000);return s<60?`${s} s`:s<3600?`${Math.round(s/60)} λεπ.`:`${(s/3600).toFixed(1)} ώρ.`;}
  function renderDiscovery(){
    const d=discData;
    if(!d||!d.available){$('discNote').textContent='Δεν είναι διαθέσιμο σε αυτή τη bridge.';$('discHueOnly').disabled=true;return;}
    $('discHueOnly').checked=Boolean(d.hue_only);
    $('discHueOnly').disabled=!d.can_edit;
    const list=d.askers||[];
    const items=list.map(a=>{
      const el=document.createElement('div');el.className='disc-item';
      const head=document.createElement('div');head.className='disc-head';
      const ip=document.createElement('strong');ip.textContent=a.ip;
      const kind=document.createElement('span');kind.textContent=a.kind==='ssdp'?'αναζήτηση SSDP':'διάβασε description.xml';
      const tag=document.createElement('span');
      if(a.kind==='ssdp'){tag.className='disc-tag '+(a.hue_search?'hue':a.answered?'':'ignored');tag.textContent=a.hue_search?'HUE':a.answered?'ΑΠΑΝΤΗΘΗΚΕ':'ΑΓΝΟΗΘΗΚΕ';}
      const cnt=document.createElement('span');cnt.textContent=`${a.count}× · τελευταία πριν ${agoText(a.last_ago_ms)}`;
      head.append(ip,kind);if(a.kind==='ssdp')head.append(tag);head.append(cnt);
      el.append(head);
      const g=guessProgram(a.ua,a.what);
      if(g){const p=document.createElement('div');p.className='disc-guess';p.textContent='→ '+g;el.append(p);}
      if(a.kind==='ssdp'){const st=document.createElement('div');st.className='disc-st';st.textContent='ST: '+a.what;el.append(st);}
      const ua=document.createElement('div');ua.className='disc-ua';ua.textContent='User-Agent: '+(a.ua&&a.ua!=='-'?a.ua:'(κανένα)');el.append(ua);
      return el;
    });
    $('discList').replaceChildren(...items);
    $('discNote').textContent=(list.length?`${list.length} διαφορετικοί από την εκκίνηση.`:'Κανείς ακόμα από την εκκίνηση.')+(d.can_edit?'':' Ο διακόπτης αλλάζει μόνο από τον browser αυτού του Linux ή μετά από «Σύνδεση».');
  }
  async function loadDiscovery(){
    try{const r=await fetch('/api/discovery',{cache:'no-store'});if(!r.ok)throw new Error();discData=await r.json();renderDiscovery();}
    catch{$('discNote').textContent='Δεν φορτώθηκε. Έλεγξε τη σύνδεση με τη live bridge.';}
  }
  $('discHueOnly').addEventListener('change',async e=>{
    const want=e.target.checked;
    try{
      const r=await fetch('/api/discovery',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hue_only:want})});
      const j=await r.json().catch(()=>({}));
      if(!r.ok){e.target.checked=!want;$('discNote').textContent='Δεν έγινε: '+(j.error==='local_browser_required'?'μόνο από τον browser αυτού του Linux ή μετά από «Σύνδεση»':(j.error||('http '+r.status)));return;}
      discData=j;renderDiscovery();
      $('discNote').textContent=want?'Αποθηκεύτηκε: απαντά μόνο σε αναζητήσεις Hue (ισχύει αμέσως).':'Αποθηκεύτηκε: απαντά σε κάθε αναζήτηση, όπως πριν.';
    }catch(err){e.target.checked=!want;$('discNote').textContent='Δεν έγινε: '+err;}
  });
  $('discRefresh').addEventListener('click',loadDiscovery);
  setInterval(()=>{if(!$('paneBridge').hidden)loadDiscovery();},5000);
  window.ambiLoadDiscovery=loadDiscovery;
  loadDiscovery();
  loadStrip(true);
  markDisconnect();
  // Live transport: Server-Sent Events (~16 updates/s), same as the old
  // Python monitor. Falls back to polling /api/state only in a browser
  // without EventSource. A dropped stream is NOT shown as disconnected
  // straight away any more: EventSource reconnects by itself, and the
  // 2.2 s watchdog below is what decides when to show "Αποσύνδεση" - a
  // single failed request used to flip the whole page to disconnected.
  async function poll(){
    try{
      const r=await fetch('/api/state',{cache:'no-store'});
      if(!r.ok)throw new Error('http '+r.status);
      render(await r.json());
    }catch(err){}
  }
  if(window.EventSource){
    const es=new EventSource('/events');
    es.addEventListener('state',e=>{try{render(JSON.parse(e.data));}catch(err){}});
  }else{
    poll();
    setInterval(poll,100);
  }
  setInterval(()=>{
    const age=Date.now()-lastEventAt;
    if(connected && age>2200)markDisconnect();
    if(connected)$('browserAge').textContent=age<1000?`${age} ms`:`${(age/1000).toFixed(1)} s`;
  },400);
  recordFreshness();
  setInterval(recordFreshness,250);
})();

</script>
</body>
</html>
)PAGE";

struct HttpRequest {
    std::string method, path, body;
    std::string host, origin, content_type;
    std::string cookie;  // v0.28: the login session
    bool ok = false;
};

HttpRequest read_request(int fd) {
    HttpRequest req;
    std::string buf;
    char chunk[4096];
    size_t header_end;
    for (;;) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return req;
        buf.append(chunk, static_cast<size_t>(n));
        header_end = buf.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (buf.size() > 65536) return req;
    }
    std::string head = buf.substr(0, header_end);
    std::string rest = buf.substr(header_end + 4);

    std::istringstream hs(head);
    std::string request_line;
    std::getline(hs, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
    std::istringstream rl(request_line);
    std::string version;
    rl >> req.method >> req.path >> version;
    if (req.method.empty() || req.path.empty()) return req;

    size_t content_length = 0;
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        size_t vs = val.find_first_not_of(' ');
        val = vs == std::string::npos ? "" : val.substr(vs);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (key == "content-length") {
            try { content_length = static_cast<size_t>(std::stoul(val)); } catch (...) { content_length = 0; }
        } else if (key == "host") {
            req.host = val;
        } else if (key == "origin") {
            req.origin = val;
        } else if (key == "content-type") {
            req.content_type = val;
        } else if (key == "cookie") {
            req.cookie = val;
        }
    }
    while (rest.size() < content_length) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return req;
        rest.append(chunk, static_cast<size_t>(n));
    }
    req.body = rest.substr(0, std::min(content_length, rest.size()));
    req.ok = true;
    return req;
}

void send_response(int fd, int code, const std::string& body, const std::string& content_type,
                   const std::string& extra_headers = "") {
    const char* status_text = code == 200 ? "OK" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                              : code == 401 ? "Unauthorized" : code == 429 ? "Too Many Requests" : "Error";
    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << " " << status_text << "\r\n"
         << extra_headers
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n"
         << body;
    std::string out = resp.str();
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
    }
}

bool send_all(int fd, const std::string& out) {
    size_t sent = 0;
    while (sent < out.size()) {
        ssize_t n = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void send_json(int fd, int code, cJSON* obj, const std::string& extra_headers = "") {
    char* text = cJSON_PrintUnformatted(obj);
    std::string body = text ? text : "{}";
    if (text) cJSON_free(text);
    cJSON_Delete(obj);
    send_response(fd, code, body, "application/json", extra_headers);
}

// Forks a detached grandchild that waits briefly (so our own HTTP response
// has time to actually leave the socket before systemd tears this process
// down) and then execs `systemctl <action> <unit>`. We deliberately don't
// wait() on it - same fire-and-forget pattern as mdns_ssdp's spawn_detached,
// for the same reason: this process may itself be gone (stopped/restarted)
// before a wait() would ever return.
void systemctl_detached(const std::string& action, const std::string& unit) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        // Detach fully: new session, so systemd stopping/restarting the
        // parent unit doesn't also signal this helper via the same cgroup
        // process group before it gets to run.
        setsid();
        pid_t grandchild = fork();
        if (grandchild != 0) _exit(0);  // first child exits immediately; init reparents the grandchild
        struct timespec ts = {1, 0};
        nanosleep(&ts, nullptr);
        execlp("systemctl", "systemctl", action.c_str(), unit.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);  // reap the short-lived first child only
}

// Builds the full ambilight-monitor/1-shaped JSON (see docs in
// jointspace_client.hpp/monitor_state.hpp) from the latest snapshot plus
// this bridge's own identity/pairing/control info - the same object GET
// /api/state always returns, which is also what the page's JS renders.
cJSON* zones_to_json(const std::vector<monitor_state::Zone>& zones) {
    cJSON* arr = cJSON_CreateArray();
    for (auto& z : zones) {
        cJSON* px = cJSON_CreateArray();
        cJSON_AddItemToArray(px, cJSON_CreateNumber(z.r));
        cJSON_AddItemToArray(px, cJSON_CreateNumber(z.g));
        cJSON_AddItemToArray(px, cJSON_CreateNumber(z.b));
        cJSON_AddItemToArray(arr, px);
    }
    return arr;
}

cJSON* build_state_json(const Options& opts, const bridge_identity::Identity& identity,
                         pairing_store::ThreadSafeStore& pairing, const monitor_state::Snapshot& snap) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema", "ambilight-monitor/1");
    cJSON_AddNumberToObject(root, "time_ms",
                             static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      std::chrono::system_clock::now().time_since_epoch())
                                                      .count()));
    cJSON_AddBoolToObject(root, "demo", false);

    cJSON* topo = cJSON_CreateObject();
    cJSON_AddNumberToObject(topo, "layers", 1);
    cJSON_AddNumberToObject(topo, "left", static_cast<double>(snap.js_left.size()));
    cJSON_AddNumberToObject(topo, "top", static_cast<double>(snap.js_top.size()));
    cJSON_AddNumberToObject(topo, "right", static_cast<double>(snap.js_right.size()));
    cJSON_AddNumberToObject(topo, "bottom", 0);
    cJSON_AddItemToObject(root, "topology", topo);

    cJSON* sides = cJSON_CreateObject();
    cJSON_AddItemToObject(sides, "left", zones_to_json(snap.js_left));
    cJSON_AddItemToObject(sides, "top", zones_to_json(snap.js_top));
    cJSON_AddItemToObject(sides, "right", zones_to_json(snap.js_right));
    cJSON_AddItemToObject(root, "sides", sides);

    cJSON* bottom = cJSON_CreateArray();
    for (size_t i = 0; i < snap.lamps.size(); ++i) {
        const auto& l = snap.lamps[i];
        cJSON* lo = cJSON_CreateObject();
        cJSON_AddStringToObject(lo, "id", std::to_string(i + 1).c_str());
        cJSON_AddStringToObject(lo, "name", l.name.c_str());
        if (snap.lamps_good) {
            cJSON* rgb = cJSON_CreateArray();
            cJSON_AddItemToArray(rgb, cJSON_CreateNumber(l.rgb.r));
            cJSON_AddItemToArray(rgb, cJSON_CreateNumber(l.rgb.g));
            cJSON_AddItemToArray(rgb, cJSON_CreateNumber(l.rgb.b));
            cJSON_AddItemToObject(lo, "rgb", rgb);
            if (l.has_xy) {
                cJSON* xy = cJSON_CreateArray();
                cJSON_AddItemToArray(xy, cJSON_CreateNumber(l.x));
                cJSON_AddItemToArray(xy, cJSON_CreateNumber(l.y));
                cJSON_AddItemToObject(lo, "xy", xy);
            } else {
                cJSON_AddNullToObject(lo, "xy");
            }
            if (l.has_bri8) {
                cJSON_AddNumberToObject(lo, "bri8", l.bri8);
            } else {
                cJSON_AddNullToObject(lo, "bri8");
            }
            cJSON* raw = cJSON_CreateArray();
            cJSON_AddItemToArray(raw, cJSON_CreateNumber(l.raw16[0]));
            cJSON_AddItemToArray(raw, cJSON_CreateNumber(l.raw16[1]));
            cJSON_AddItemToArray(raw, cJSON_CreateNumber(l.raw16[2]));
            cJSON_AddItemToObject(lo, "raw16", raw);
            cJSON_AddNumberToObject(lo, "age_ms", std::max(0.0, snap.hue_age_s) * 1000.0);
        } else {
            cJSON_AddNullToObject(lo, "rgb");
            cJSON_AddNullToObject(lo, "xy");
            cJSON_AddNullToObject(lo, "bri8");
            cJSON_AddNullToObject(lo, "raw16");
            cJSON_AddNullToObject(lo, "age_ms");
        }
        cJSON_AddItemToArray(bottom, lo);
    }
    cJSON_AddItemToObject(root, "bottom", bottom);

    cJSON* sources = cJSON_CreateObject();
    cJSON* js = cJSON_CreateObject();
    cJSON_AddStringToObject(js, "status", snap.js_status.c_str());
    if (snap.js_error.empty())
        cJSON_AddNullToObject(js, "error");
    else
        cJSON_AddStringToObject(js, "error", snap.js_error.c_str());
    if (snap.js_age_s >= 0)
        cJSON_AddNumberToObject(js, "age_ms", snap.js_age_s * 1000.0);
    else
        cJSON_AddNullToObject(js, "age_ms");
    cJSON_AddNumberToObject(js, "fps", snap.js_fps);
    cJSON_AddItemToObject(sources, "jointspace", js);

    cJSON* hue = cJSON_CreateObject();
    const char* hue_status = !snap.stream_active        ? "inactive"
                              : snap.lamps_good          ? "live"
                              : snap.hue_age_s < 0        ? "waiting"
                                                           : "stale";
    cJSON_AddStringToObject(hue, "status", hue_status);
    if (snap.hue_age_s >= 0)
        cJSON_AddNumberToObject(hue, "age_ms", snap.hue_age_s * 1000.0);
    else
        cJSON_AddNullToObject(hue, "age_ms");
    cJSON_AddNumberToObject(hue, "fps", snap.hue_fps);
    cJSON_AddNumberToObject(hue, "frames", snap.hue_frames);
    cJSON_AddNumberToObject(hue, "bad_frames", snap.hue_bad_frames);
    if (snap.hue_version.empty())
        cJSON_AddNullToObject(hue, "version");
    else
        cJSON_AddStringToObject(hue, "version", snap.hue_version.c_str());
    if (snap.hue_colorspace < 0)
        cJSON_AddNullToObject(hue, "colorspace");
    else
        cJSON_AddNumberToObject(hue, "colorspace", snap.hue_colorspace);
    if (snap.old_session) {
        cJSON* old = cJSON_CreateObject();
        cJSON_AddStringToObject(old, "ip", snap.old_session_ip.c_str());
        cJSON_AddNumberToObject(old, "packets", static_cast<double>(snap.old_session_packets));
        cJSON_AddItemToObject(hue, "old_session", old);
    } else {
        cJSON_AddNullToObject(hue, "old_session");
    }
    if (snap.tv_dtls_lock) {
        cJSON* lock = cJSON_CreateObject();
        cJSON_AddStringToObject(lock, "reason", snap.tv_dtls_lock_reason.c_str());
        cJSON_AddStringToObject(lock, "detail", snap.tv_dtls_lock_detail.c_str());
        cJSON_AddNumberToObject(lock, "age_s", snap.tv_dtls_lock_age_s);
        cJSON_AddBoolToObject(lock, "confirmed", snap.tv_dtls_lock_confirmed);
        cJSON_AddItemToObject(hue, "tv_dtls_lock", lock);
    } else {
        cJSON_AddNullToObject(hue, "tv_dtls_lock");
    }
    if (snap.last_handshake_latency_s < 0)
        cJSON_AddNullToObject(hue, "handshake_latency_s");
    else
        cJSON_AddNumberToObject(hue, "handshake_latency_s", snap.last_handshake_latency_s);
    cJSON_AddItemToObject(sources, "hue", hue);
    cJSON_AddItemToObject(root, "sources", sources);

    // v0.12: the bottom strip exactly as computed and sent to WLED (before
    // v0.12 this was a browser-side simulation and "available" was false).
    cJSON* output = cJSON_CreateObject();
    cJSON_AddBoolToObject(output, "available", !snap.strip.empty());
    if (snap.strip.empty()) {
        cJSON_AddNullToObject(output, "leds");
    } else {
        cJSON* leds = cJSON_CreateArray();
        for (const auto& c : snap.strip) {
            cJSON* px = cJSON_CreateArray();
            cJSON_AddItemToArray(px, cJSON_CreateNumber(c.r));
            cJSON_AddItemToArray(px, cJSON_CreateNumber(c.g));
            cJSON_AddItemToArray(px, cJSON_CreateNumber(c.b));
            cJSON_AddItemToArray(leds, px);
        }
        cJSON_AddItemToObject(output, "leds", leds);
    }
    cJSON_AddStringToObject(output, "space", "led_pwm_linear");
    cJSON_AddNumberToObject(output, "estimated_ma", snap.strip.empty() ? 0.0 : snap.estimated_ma);
    const auto& si = snap.strip_info;
    cJSON_AddNumberToObject(output, "budget_ma", si.budget_ma);
    cJSON_AddNumberToObject(output, "ma_per_led_full_white", si.ma_per_led_full_white);
    cJSON_AddNumberToObject(output, "used_anchors", snap.used_anchors);
    // v0.13 (Phase 2): how the budget was enforced on this frame.
    cJSON* lim = cJSON_CreateObject();
    cJSON_AddStringToObject(lim, "mode", si.limit_mode.c_str());
    cJSON_AddNumberToObject(lim, "peak_emphasis", si.peak_emphasis);
    cJSON_AddNumberToObject(lim, "peak_scale", si.peak_scale);
    cJSON_AddNumberToObject(lim, "min_scale", si.min_scale);
    cJSON* pk = cJSON_CreateArray();
    for (int p : si.peak_leds) cJSON_AddItemToArray(pk, cJSON_CreateNumber(p));
    cJSON_AddItemToObject(lim, "peak_leds", pk);
    cJSON_AddNumberToObject(lim, "rise_ms", si.rise_ms);
    cJSON_AddNumberToObject(lim, "lamp_inset", si.lamp_inset);
    cJSON_AddStringToObject(lim, "blend_mode", si.blend_mode.c_str());
    cJSON_AddStringToObject(lim, "corner_mode", si.corner_mode.c_str());
    cJSON_AddNumberToObject(lim, "blend_softness", si.blend_softness);
    cJSON_AddNumberToObject(lim, "peak_drift_leds", si.peak_drift_leds);
    cJSON_AddNumberToObject(lim, "peak_offset_leds", std::round(si.peak_offset_leds * 100) / 100);
    cJSON_AddNumberToObject(lim, "wear_strength", si.wear_strength);  // v0.33
    cJSON_AddBoolToObject(lim, "wear_applied", si.wear_applied);
    cJSON_AddBoolToObject(lim, "peak_drift_auto", si.peak_drift_auto);       // v0.24
    cJSON_AddStringToObject(lim, "peak_drift_shape", si.peak_drift_shape.c_str());
    cJSON_AddBoolToObject(lim, "end_lamps", si.end_lamps);
    cJSON_AddStringToObject(lim, "strip_layout", si.strip_layout.c_str());  // v0.30
    cJSON_AddNumberToObject(lim, "edge_leds", si.edge_leds);
    cJSON_AddNumberToObject(lim, "edge_strength", si.edge_strength);
    cJSON_AddStringToObject(lim, "edge_source", si.edge_source.c_str());
    cJSON_AddStringToObject(lim, "edge_source_setting", si.edge_source_setting.c_str());
    cJSON_AddNumberToObject(lim, "corner_weight", std::round(si.corner_weight * 100) / 100);
    cJSON_AddStringToObject(lim, "output_phase", si.output_phase.c_str());
    cJSON_AddNumberToObject(lim, "wanted_ma", snap.strip.empty() ? 0.0 : std::round(si.wanted_ma));
    cJSON_AddNumberToObject(lim, "target_ma", snap.strip.empty() ? 0.0 : si.target_ma);
    cJSON_AddItemToObject(output, "limit", lim);
    if (snap.strip.empty() || si.compare_pe0.empty()) {
        cJSON_AddNullToObject(output, "compare_pe0");
    } else {
        cJSON* cmp = cJSON_CreateObject();
        cJSON* cl = cJSON_CreateArray();
        for (const auto& c : si.compare_pe0) {
            cJSON* px = cJSON_CreateArray();
            cJSON_AddItemToArray(px, cJSON_CreateNumber(c.r));
            cJSON_AddItemToArray(px, cJSON_CreateNumber(c.g));
            cJSON_AddItemToArray(px, cJSON_CreateNumber(c.b));
            cJSON_AddItemToArray(cl, px);
        }
        cJSON_AddItemToObject(cmp, "leds", cl);
        cJSON_AddNumberToObject(cmp, "estimated_ma", si.compare_pe0_ma);
        cJSON_AddItemToObject(output, "compare_pe0", cmp);
    }
    cJSON_AddStringToObject(output, "note",
                            "computed by the bridge (render_strip), linear LED PWM values; estimate, not measured");
    cJSON* wled = cJSON_CreateObject();
    cJSON_AddBoolToObject(wled, "enabled", !snap.wled_target.empty());
    if (snap.wled_target.empty())
        cJSON_AddNullToObject(wled, "target");
    else
        cJSON_AddStringToObject(wled, "target", snap.wled_target.c_str());
    cJSON_AddNumberToObject(wled, "fps", snap.wled_fps);
    cJSON_AddNumberToObject(wled, "frames", static_cast<double>(snap.wled_frames));
    cJSON_AddNumberToObject(wled, "errors", static_cast<double>(snap.wled_errors));
    if (snap.wled_last_error.empty())
        cJSON_AddNullToObject(wled, "last_error");
    else
        cJSON_AddStringToObject(wled, "last_error", snap.wled_last_error.c_str());
    cJSON_AddItemToObject(output, "wled", wled);
    cJSON_AddItemToObject(root, "output", output);

    // Extra fields - not part of the old Python prototype's contract, used
    // only by this bridge's own "Bridge" panel on the page.
    cJSON* ident = cJSON_CreateObject();
    cJSON_AddStringToObject(ident, "name", identity.name.c_str());
    cJSON_AddStringToObject(ident, "mac", identity.mac.c_str());
    cJSON_AddStringToObject(ident, "bridgeid", identity.bridgeid.c_str());
    cJSON_AddStringToObject(ident, "ip", identity.ip.c_str());
    cJSON_AddItemToObject(root, "identity", ident);
    cJSON_AddNumberToObject(root, "pairing_known_users", static_cast<double>(pairing.user_count()));
    cJSON_AddBoolToObject(root, "control_enabled", !opts.systemd_unit.empty());
    // v0.24: the version, and whether Restart works without systemd (in place)
    cJSON_AddStringToObject(root, "bridge_version", opts.version.c_str());
    cJSON_AddBoolToObject(root, "restart_available", opts.systemd_unit.empty() && bool(opts.request_restart));
    // v0.28: service state + the in-place Start/Stop/Restart/Exit controls
    cJSON_AddStringToObject(root, "service", opts.service_state ? opts.service_state().c_str() : "running");
    cJSON_AddBoolToObject(root, "service_controls", opts.systemd_unit.empty() && bool(opts.request_service));
    cJSON* link = cJSON_CreateObject();
    cJSON_AddBoolToObject(link, "required", opts.pairing_requires_button);
    cJSON_AddNumberToObject(link, "remaining_ms", static_cast<double>(pairing.link_remaining_ms()));
    // v0.15: "the TV wants to pair" - the TV's POST /api was refused with
    // 101 in the last 30 s and no pairing has succeeded since.
    auto pr = pairing.pair_request_state();
    cJSON_AddBoolToObject(link, "tv_waiting", pr.tv_waiting);
    if (pr.refused_ago_ms < 0) cJSON_AddNullToObject(link, "refused_ago_ms");
    else cJSON_AddNumberToObject(link, "refused_ago_ms", static_cast<double>(pr.refused_ago_ms));
    if (pr.paired_ago_ms < 0) cJSON_AddNullToObject(link, "paired_ago_ms");
    else cJSON_AddNumberToObject(link, "paired_ago_ms", static_cast<double>(pr.paired_ago_ms));
    cJSON_AddItemToObject(root, "link_button", link);
    return root;
}

// Same "is this actually a request from this Linux machine's own browser"
// check as the Python original's _settings_local(): the POST that writes
// JointSpace credentials is only ever honoured for a request whose Host
// header, Origin header and peer address all agree this is a same-origin
// request from localhost - never a LAN client, even on the bridge's own
// monitor port.
bool is_local_browser(const sockaddr_in& peer, const HttpRequest& req, uint16_t port) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    if (std::string(ip) != "127.0.0.1") return false;
    std::string expect1 = "localhost:" + std::to_string(port);
    std::string expect2 = "127.0.0.1:" + std::to_string(port);
    std::string host = req.host;
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    return host == expect1 || host == expect2;
}

// ---- v0.28: login for other devices ------------------------------------
const char* kCookieName = "ambi_session";

std::string session_token(const HttpRequest& req) {
    // "a=1; ambi_session=<64 hex>; b=2"
    const std::string key = std::string(kCookieName) + "=";
    size_t pos = 0;
    while (pos < req.cookie.size()) {
        size_t end = req.cookie.find(';', pos);
        if (end == std::string::npos) end = req.cookie.size();
        std::string part = req.cookie.substr(pos, end - pos);
        size_t a = part.find_first_not_of(' ');
        if (a != std::string::npos) part = part.substr(a);
        if (part.compare(0, key.size(), key) == 0) return part.substr(key.size());
        pos = end + 1;
    }
    return "";
}

int64_t wall_now_s() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string peer_ip(const sockaddr_in& peer) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    return ip;
}

bool logged_in(const Options& opts, const HttpRequest& req) {
    return opts.login && opts.login->valid(session_token(req), wall_now_s());
}

// May this request see the edit controls? This Linux machine's own browser,
// or a browser that logged in (v0.28).
bool can_edit(const Options& opts, const sockaddr_in& peer, const HttpRequest& req) {
    return is_local_browser(peer, req, opts.port) || logged_in(opts, req);
}

// May this POST change something? can_edit + the same-origin check
// (a cross-site form or fetch never carries Origin = http://<our Host>).
bool can_write(const Options& opts, const sockaddr_in& peer, const HttpRequest& req) {
    return can_edit(opts, peer, req) && req.origin == "http://" + req.host;
}

cJSON* login_json(const Options& opts, const sockaddr_in& peer, const HttpRequest& req) {
    cJSON* o = cJSON_CreateObject();
    const bool local = is_local_browser(peer, req, opts.port);
    const bool in = logged_in(opts, req);
    cJSON_AddBoolToObject(o, "available", opts.login != nullptr);
    cJSON_AddBoolToObject(o, "configured", opts.login && opts.login->configured());
    cJSON_AddBoolToObject(o, "local", local);
    cJSON_AddBoolToObject(o, "logged_in", in);
    cJSON_AddBoolToObject(o, "can_edit", local || in);
    if ((local || in) && opts.login && opts.login->configured())
        cJSON_AddStringToObject(o, "username", opts.login->username().c_str());
    cJSON_AddNumberToObject(o, "session_days", static_cast<double>(login_store::kSessionSeconds / 86400));
    return o;
}

cJSON* jointspace_settings_json(const jointspace::Settings::Status& st, bool can_edit) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", st.available);
    cJSON_AddBoolToObject(o, "configured", st.configured);
    cJSON_AddBoolToObject(o, "persisted", st.persisted);
    cJSON_AddBoolToObject(o, "load_error", st.load_error);
    cJSON_AddBoolToObject(o, "can_edit", can_edit);
    return o;
}

cJSON* identity_json(identity_store::Shared* ids, bool can_edit) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", ids != nullptr);
    cJSON_AddBoolToObject(o, "can_edit", can_edit && ids != nullptr);
    if (!ids) return o;
    identity_store::Store st = ids->snapshot();
    const identity_store::Entry& run = ids->running();
    const identity_store::Entry* act = st.find(st.active);
    cJSON* r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "mac", run.mac.c_str());
    cJSON_AddStringToObject(r, "name", run.name.c_str());
    cJSON_AddNumberToObject(r, "lamps", run.lamps);
    cJSON_AddItemToObject(o, "running", r);
    cJSON_AddStringToObject(o, "active", st.active.c_str());
    // Saved choice differs from what this process runs with -> restart.
    bool restart = !act || act->mac != run.mac || act->name != run.name || act->lamps != run.lamps;
    cJSON_AddBoolToObject(o, "restart_needed", restart);
    cJSON* list = cJSON_AddArrayToObject(o, "identities");
    for (const auto& e : st.entries) {
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "mac", e.mac.c_str());
        cJSON_AddStringToObject(it, "name", e.name.c_str());
        cJSON_AddNumberToObject(it, "lamps", e.lamps);
        auto bid = bridge_identity::make_identity("", 0, e.mac, e.name).bridgeid;
        cJSON_AddStringToObject(it, "bridgeid", bid.c_str());
        cJSON_AddBoolToObject(it, "default", e.mac == bridge_identity::kDefaultMac);
        cJSON_AddBoolToObject(it, "active", e.mac == st.active);
        cJSON_AddBoolToObject(it, "running", e.mac == run.mac);
        // How many TVs/apps paired with this identity (its own pairing.json).
        std::string pp = identity_store::dir_for(ids->state_dir(), e.mac) + "/pairing.json";
        cJSON_AddNumberToObject(it, "paired",
                                static_cast<double>(pairing_store::Store::load(pp).users().size()));
        cJSON_AddItemToArray(list, it);
    }
    return o;
}

cJSON* discovery_json(discovery::Registry* d, bool can_edit) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", d != nullptr);
    cJSON_AddBoolToObject(o, "can_edit", can_edit && d != nullptr);
    if (!d) return o;
    cJSON_AddBoolToObject(o, "hue_only", d->hue_only());
    int64_t now = discovery::now_ms();
    cJSON_AddNumberToObject(o, "now_ms", static_cast<double>(now));
    cJSON* list = cJSON_AddArrayToObject(o, "askers");
    for (const auto& a : d->snapshot()) {
        cJSON* it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ip", a.ip.c_str());
        cJSON_AddStringToObject(it, "kind", a.kind.c_str());
        cJSON_AddStringToObject(it, "what", a.what.c_str());
        cJSON_AddStringToObject(it, "ua", a.ua.c_str());
        cJSON_AddNumberToObject(it, "count", static_cast<double>(a.count));
        cJSON_AddNumberToObject(it, "first_ago_ms", static_cast<double>(now - a.first_ms));
        cJSON_AddNumberToObject(it, "last_ago_ms", static_cast<double>(now - a.last_ms));
        cJSON_AddBoolToObject(it, "answered", a.answered);
        cJSON_AddBoolToObject(it, "hue_search", !discovery::answer_variants(a.what, "", true).empty() ||
                                                    a.what.rfind("uuid:", 0) == 0);
        cJSON_AddItemToArray(list, it);
    }
    return o;
}

cJSON* strip_settings_json(settings::SharedSettings* strip, bool can_edit) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", strip != nullptr);
    cJSON_AddBoolToObject(o, "can_edit", can_edit && strip != nullptr);
    if (strip) cJSON_AddItemToObject(o, "settings", settings::to_json(strip->get()));
    cJSON_AddItemToObject(o, "defaults", settings::to_json(color_pipeline::Settings{}));
    // v0.33: limits the forms must not hard-code (the ceiling is fixed at build time)
    cJSON* limits = cJSON_CreateObject();
    cJSON_AddNumberToObject(limits, "current_budget_ma_max", color_pipeline::kMaxBudgetMa);
    cJSON_AddItemToObject(o, "limits", limits);
    return o;
}

}  // namespace

void run_server(const Options& opts, const bridge_identity::Identity& identity,
                 pairing_store::ThreadSafeStore& pairing, monitor_state::SharedState& state,
                 jointspace::Settings& js_settings, settings::SharedSettings* strip_settings,
                 identity_store::Shared* identities, discovery::Registry* discovery,
                 strip_stats::Stats* strip_stats) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::fprintf(stderr, "[monitor] socket() failed: %s\n", std::strerror(errno));
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(opts.port);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[monitor] bind 0.0.0.0:%u failed: %s -> monitor page disabled\n", opts.port,
                     std::strerror(errno));
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 8) != 0) {
        std::fprintf(stderr, "[monitor] listen() failed: %s\n", std::strerror(errno));
        close(listen_fd);
        return;
    }
    std::fprintf(stderr, "[monitor] dashboard on http://0.0.0.0:%u/ (systemd unit: %s)\n", opts.port,
                 opts.systemd_unit.empty() ? "none - Stop/Restart buttons disabled" : opts.systemd_unit.c_str());

    // One thread per connection, like the old Python monitor's
    // ThreadingHTTPServer. Until v5 this was a single serial accept loop
    // with a blocking recv() and no timeout: one connection that opened
    // but never sent a request - which browsers do routinely (Chrome's
    // speculative "preconnect" sockets, a half-dead tab, a phone that went
    // to sleep mid-request) - blocked every other request indefinitely,
    // so the page sat at "Αποσύνδεση" / took ages to connect until that
    // socket finally died. It also made a long-lived SSE stream (below)
    // impossible. Everything a handler touches is already thread-safe:
    // `state` and `pairing` lock internally, `js_settings` locks
    // internally, `opts`/`identity` are read-only.
    auto handle = [&opts, &identity, &pairing, &state, &js_settings, strip_settings, identities, discovery, strip_stats](int fd, sockaddr_in peer) {
        HttpRequest req = read_request(fd);
        if (!req.ok) {
            close(fd);
            return;
        }
        std::string path = req.path;
        size_t qpos = path.find('?');
        if (qpos != std::string::npos) path = path.substr(0, qpos);

        if (req.method == "GET" && (path == "/" || path == "/monitor")) {
            send_response(fd, 200, kPage, "text/html; charset=utf-8");
        } else if (req.method == "GET" && path == "/ambi/settings") {
            // v0.32: the strip settings as a page of their own
            send_response(fd, 200, settings_page::kPage, "text/html; charset=utf-8");
        } else if (req.method == "POST" && path == "/api/settings/strip/validated") {
            // v0.32: like POST /api/settings/strip, but every value is
            // checked first and NOTHING is saved when one is wrong (422 +
            // the list of problems). The older route keeps clamping quietly.
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else if (!strip_settings) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "not_available");
                send_json(fd, 409, e);
            } else if (!cJSON_IsObject(body)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "invalid_json");
                send_json(fd, 400, e);
            } else {
                const std::vector<settings::ValidationError> errs = settings::validate_json(body);
                if (!errs.empty()) {
                    cJSON* e = cJSON_CreateObject();
                    cJSON_AddStringToObject(e, "error", "invalid_values");
                    cJSON* list = cJSON_CreateArray();
                    for (const auto& er : errs) {
                        cJSON* it = cJSON_CreateObject();
                        cJSON_AddStringToObject(it, "field", er.field.c_str());
                        cJSON_AddStringToObject(it, "message", er.message.c_str());
                        cJSON_AddItemToArray(list, it);
                    }
                    cJSON_AddItemToObject(e, "errors", list);
                    std::fprintf(stderr, "[monitor] settings page: %zu value(s) refused, nothing saved (first: %s %s)\n",
                                 errs.size(), errs[0].field.c_str(), errs[0].message.c_str());
                    send_json(fd, 422, e);
                } else {
                    color_pipeline::Settings s = strip_settings->get();
                    settings::merge_json(s, body);
                    const bool saved = strip_settings->update(s);
                    std::fprintf(stderr,
                                 "[monitor] settings page: leds=%d budget=%.0f mA mA/LED=%.1f brightness=%.2f rise=%.0f ms "
                                 "pe=%.2f layout=%s lamp_positions=%zu (saved=%s)\n",
                                 s.led_count, s.current_budget_ma, s.ma_per_led_full_white, s.brightness, s.rise_ms,
                                 s.peak_emphasis, color_pipeline::strip_layout_name(s.strip_layout),
                                 s.lamp_positions.size(), saved ? "yes" : "NO, kept in memory only");
                    cJSON* o = strip_settings_json(strip_settings, true);
                    cJSON_AddBoolToObject(o, "saved_to_disk", saved);
                    send_json(fd, 200, o);
                }
            }
            if (body) cJSON_Delete(body);
        } else if (req.method == "GET" && path == "/events") {
            // Server-Sent Events, same as the old Python monitor's /events:
            // push a fresh snapshot every ~60 ms (~16 updates/s) over one
            // long-lived connection, instead of the page polling
            // /api/state every 250 ms (4 updates/s) - the polling version
            // was visibly laggier than the old monitor on the same TV.
            // Ends as soon as a send fails (tab closed/reloaded: EPIPE,
            // not SIGPIPE - see main.cpp) or the send timeout expires.
            std::string head =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n";
            if (send_all(fd, head)) {
                for (;;) {
                    monitor_state::Snapshot snap = state.get();
                    cJSON* obj = build_state_json(opts, identity, pairing, snap);
                    char* text = cJSON_PrintUnformatted(obj);
                    cJSON_Delete(obj);
                    std::string msg = std::string("event: state\ndata: ") + (text ? text : "{}") + "\n\n";
                    if (text) cJSON_free(text);
                    if (!send_all(fd, msg)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(60));
                }
            }
        } else if (req.method == "GET" && path == "/api/state") {
            monitor_state::Snapshot snap = state.get();
            send_json(fd, 200, build_state_json(opts, identity, pairing, snap));
        } else if (req.method == "GET" && path == "/api/settings/jointspace") {
            send_json(fd, 200, jointspace_settings_json(js_settings.status(), can_edit(opts, peer, req)));
        } else if (req.method == "POST" && path == "/api/settings/jointspace") {
            // Same gate as the GET above, plus the Origin check the old
            // Python prototype used: a same-origin fetch() from the page
            // itself sets Origin to "http://<Host>", which a cross-site
            // request (or curl from elsewhere on the LAN) won't have.
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else {
                cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
                cJSON* u = body ? cJSON_GetObjectItemCaseSensitive(body, "username") : nullptr;
                cJSON* p = body ? cJSON_GetObjectItemCaseSensitive(body, "password") : nullptr;
                std::string err;
                if (!cJSON_IsString(u) || !cJSON_IsString(p) ||
                    !js_settings.update(u->valuestring, p->valuestring, err)) {
                    cJSON* e = cJSON_CreateObject();
                    cJSON_AddStringToObject(e, "error", err.empty() ? "invalid_credentials_or_settings" : err.c_str());
                    send_json(fd, 400, e);
                } else {
                    // Neither the request body nor the credentials are ever
                    // echoed back - same as the Python original.
                    send_json(fd, 200, jointspace_settings_json(js_settings.status(), true));
                }
                if (body) cJSON_Delete(body);
            }
        } else if (req.method == "GET" && path == "/api/strip_stats") {
            // v0.23 (NEXT 6): per-LED log + the limiter's compromise episodes. Read-only for anyone.
            const double now_s =
                std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            cJSON* j = strip_stats ? strip_stats->to_json(now_s) : cJSON_CreateObject();
            if (!strip_stats) cJSON_AddBoolToObject(j, "available", false);
            cJSON_AddBoolToObject(j, "can_edit", can_edit(opts, peer, req));
            send_json(fd, 200, j);
        } else if (req.method == "POST" && path == "/api/strip_stats/reset") {
            // Clears only "since reset"; all-time totals (real wear) are never reset from the page.
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else if (!strip_stats) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "not_available");
                send_json(fd, 409, e);
            } else {
                const double now_s =
                    std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                strip_stats->reset_since(now_s);
                std::fprintf(stderr, "[stats] \"since reset\" totals cleared from the page\n");
                cJSON* j = strip_stats->to_json(now_s);
                cJSON_AddBoolToObject(j, "can_edit", true);
                send_json(fd, 200, j);
            }
        } else if (req.method == "POST" && path == "/api/strip_stats/install") {
            // v0.41 (queue item 5): «the strip was mounted / replaced now» -> since_install starts;
            // the wear-aware limiter reads only that set.
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else if (!strip_stats) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "not_available");
                send_json(fd, 409, e);
            } else {
                const double now_s =
                    std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
                strip_stats->reset_install(now_s);
                std::fprintf(stderr, "[stats] strip marked as mounted now: \"since install\" totals started\n");
                cJSON* j = strip_stats->to_json(now_s);
                cJSON_AddBoolToObject(j, "can_edit", true);
                send_json(fd, 200, j);
            }
        } else if (req.method == "GET" && path == "/api/settings/strip") {
            // v0.15 (Phase 3): strip/current settings. Anyone may read them,
            // only this machine's own browser may change them (same gate
            // as the JointSpace credentials just above).
            send_json(fd, 200, strip_settings_json(strip_settings, can_edit(opts, peer, req)));
        } else if (req.method == "POST" && path == "/api/settings/strip") {
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else if (!strip_settings) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "not_available");
                send_json(fd, 409, e);
            } else if (!cJSON_IsObject(body)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "invalid_json");
                send_json(fd, 400, e);
            } else {
                color_pipeline::Settings s = strip_settings->get();  // partial merge, clamped
                settings::merge_json(s, body);
                bool saved = strip_settings->update(s);
                std::fprintf(stderr,
                             "[monitor] strip settings changed from the page: leds=%d budget=%.0f mA "
                             "mA/LED=%.1f idle=%.2f pe=%.2f rise=%.0f ms inset=%.2f (saved=%s)\n",
                             s.led_count, s.current_budget_ma, s.ma_per_led_full_white, s.idle_ma_per_led,
                             s.peak_emphasis, s.rise_ms, s.lamp_inset, saved ? "yes" : "NO, kept in memory only");
                cJSON* o = strip_settings_json(strip_settings, true);
                cJSON_AddBoolToObject(o, "saved_to_disk", saved);
                send_json(fd, 200, o);
            }
            if (body) cJSON_Delete(body);
        } else if (req.method == "GET" && path == "/api/discovery") {
            send_json(fd, 200, discovery_json(discovery, can_edit(opts, peer, req)));
        } else if (req.method == "POST" && path == "/api/discovery") {
            // v0.19: "answer only Hue searches" switch, local browser only.
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            cJSON* h = body ? cJSON_GetObjectItemCaseSensitive(body, "hue_only") : nullptr;
            std::string err;
            if (!can_write(opts, peer, req)) {
                err = "local_browser_required";
            } else if (!discovery) {
                err = "not_available";
            } else if (!cJSON_IsBool(h)) {
                err = "hue_only_must_be_true_or_false";
            } else if (discovery->set_hue_only(cJSON_IsTrue(h), &err)) {
                std::fprintf(stderr, "[monitor] SSDP: answer %s (set from the local page)\n",
                             cJSON_IsTrue(h) ? "only Hue searches" : "every search");
            }
            if (err.empty()) {
                send_json(fd, 200, discovery_json(discovery, true));
            } else {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", err.c_str());
                send_json(fd, err == "local_browser_required" ? 403 : 400, e);
            }
            if (body) cJSON_Delete(body);
        } else if (req.method == "GET" && path == "/api/identity") {
            send_json(fd, 200, identity_json(identities, can_edit(opts, peer, req)));
        } else if (req.method == "POST" && path == "/api/identity") {
            // v0.17: same local-browser gate as the other settings.
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            auto str = [body](const char* k) {
                cJSON* v = body ? cJSON_GetObjectItemCaseSensitive(body, k) : nullptr;
                return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string();
            };
            std::string err;
            bool ok = false;
            if (!can_write(opts, peer, req)) {
                err = "local_browser_required";
            } else if (!identities) {
                err = "not_available";
            } else {
                std::string action = str("action");
                if (action == "select") {
                    ok = identities->select(str("mac"), &err);
                } else if (action == "rename") {
                    ok = identities->rename(str("mac"), str("name"), &err);
                } else if (action == "lamps") {
                    cJSON* l = body ? cJSON_GetObjectItemCaseSensitive(body, "lamps") : nullptr;
                    ok = identities->set_lamps(str("mac"), cJSON_IsNumber(l) ? l->valueint : 0, &err);
                } else if (action == "create") {
                    std::string mac;
                    ok = identities->create(str("name"), &mac, &err);
                } else {
                    err = "unknown_action";
                }
                std::fprintf(stderr, "[monitor] identity %s %s%s\n", action.c_str(), ok ? "ok" : "refused: ",
                             ok ? "" : err.c_str());
            }
            if (ok) {
                send_json(fd, 200, identity_json(identities, true));
            } else {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", err.c_str());
                send_json(fd, err == "local_browser_required" ? 403 : 400, e);
            }
            if (body) cJSON_Delete(body);
        } else if (req.method == "POST" && path == "/api/pair") {
            // Virtual link button (v9). Same LAN trust model as Stop/Restart
            // (see monitor_server.hpp): anyone who can reach this port can
            // press it - which is also true of a real bridge's physical
            // button for anyone in the room.
            pairing.press_link_button(std::chrono::seconds(30));
            char ip[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            std::fprintf(stderr, "[monitor] link button pressed from %s - pairing open for 30 s\n", ip);
            cJSON* ok = cJSON_CreateObject();
            cJSON_AddBoolToObject(ok, "ok", true);
            cJSON_AddNumberToObject(ok, "remaining_ms", static_cast<double>(pairing.link_remaining_ms()));
            send_json(fd, 200, ok);
        } else if (req.method == "POST" && path == "/api/restart") {
            // v0.24 (PLAN item 4): restart in place (same arguments, newest
            // build) - only from this Linux machine's own browser, like the
            // other changes. Under systemd the page uses /api/control.
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else if (!opts.request_restart || !opts.systemd_unit.empty()) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "not_available");
                send_json(fd, 409, e);
            } else {
                std::string err = opts.request_restart("monitor page");
                cJSON* o = cJSON_CreateObject();
                cJSON_AddBoolToObject(o, "ok", err.empty());
                if (!err.empty()) cJSON_AddStringToObject(o, "error", err.c_str());
                send_json(fd, err.empty() ? 200 : 500, o);
            }
        } else if (req.method == "POST" && path == "/api/service") {
            // v0.28: Start / Stop / Restart / Exit (same gate as the settings:
            // this Linux machine's browser, or logged in).
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            cJSON* a = body ? cJSON_GetObjectItemCaseSensitive(body, "action") : nullptr;
            std::string action = cJSON_IsString(a) && a->valuestring ? a->valuestring : "";
            if (body) cJSON_Delete(body);
            if (!can_write(opts, peer, req)) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "local_browser_required");
                send_json(fd, 403, e);
            } else if (!opts.request_service || !opts.systemd_unit.empty()) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "not_available");
                send_json(fd, 409, e);
            } else if (action != "start" && action != "stop" && action != "restart" && action != "exit") {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "action_must_be_start_stop_restart_exit");
                send_json(fd, 400, e);
            } else {
                std::string err = opts.request_service(action, "monitor page (" + peer_ip(peer) + ")");
                cJSON* o = cJSON_CreateObject();
                cJSON_AddBoolToObject(o, "ok", err.empty());
                cJSON_AddStringToObject(o, "action", action.c_str());
                if (!err.empty()) cJSON_AddStringToObject(o, "error", err.c_str());
                send_json(fd, err.empty() ? 200 : 409, o);
            }
        } else if (req.method == "GET" && path == "/api/login") {
            send_json(fd, 200, login_json(opts, peer, req));
        } else if (req.method == "POST" && path == "/api/login") {
            // v0.28: log in from another device. Same-origin only (no login CSRF).
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            cJSON* u = body ? cJSON_GetObjectItemCaseSensitive(body, "username") : nullptr;
            cJSON* pw = body ? cJSON_GetObjectItemCaseSensitive(body, "password") : nullptr;
            std::string user = cJSON_IsString(u) && u->valuestring ? u->valuestring : "";
            std::string pass = cJSON_IsString(pw) && pw->valuestring ? pw->valuestring : "";
            if (body) cJSON_Delete(body);
            const std::string ip = peer_ip(peer);
            std::string err, token;
            if (!opts.login) err = "not_available";
            else if (req.origin != "http://" + req.host) err = "same_origin_required";
            else token = opts.login->login(user, pass, ip, wall_now_s(), &err);
            if (!token.empty()) {
                std::fprintf(stderr, "[monitor] login ok from %s\n", ip.c_str());
                std::string cookie = std::string("Set-Cookie: ") + kCookieName + "=" + token +
                                     "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
                                     std::to_string(login_store::kSessionSeconds) + "\r\n";
                cJSON* o = cJSON_CreateObject();
                cJSON_AddBoolToObject(o, "ok", true);
                send_json(fd, 200, o, cookie);
            } else {
                const bool locked = err.rfind("locked:", 0) == 0;
                std::fprintf(stderr, "[monitor] login refused from %s (%s)\n", ip.c_str(), err.c_str());
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", locked ? "locked" : err.c_str());
                if (locked) cJSON_AddNumberToObject(e, "retry_s", std::atof(err.c_str() + 7));
                send_json(fd, locked ? 429 : err == "bad_credentials" ? 401 : err == "same_origin_required" ? 403 : 409, e);
            }
        } else if (req.method == "POST" && path == "/api/logout") {
            if (opts.login && req.origin == "http://" + req.host) opts.login->logout(session_token(req));
            cJSON* o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", true);
            send_json(fd, 200, o,
                      std::string("Set-Cookie: ") + kCookieName + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0\r\n");
        } else if (req.method == "POST" && path == "/api/login/setup") {
            // v0.28: set / change / remove the login - ONLY from this Linux
            // machine's own browser (a logged-in device cannot change it).
            cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
            auto str = [body](const char* k) {
                cJSON* v = body ? cJSON_GetObjectItemCaseSensitive(body, k) : nullptr;
                return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string();
            };
            std::string action = str("action"), err;
            bool ok = false;
            if (!is_local_browser(peer, req, opts.port) || req.origin != "http://" + req.host) {
                err = "local_browser_required";
            } else if (!opts.login) {
                err = "not_available";
            } else if (action == "set") {
                ok = opts.login->set_credentials(str("username"), str("password"), &err);
                std::fprintf(stderr, "[monitor] monitor login %s%s\n", ok ? "set for user " : "not set: ",
                             ok ? str("username").c_str() : err.c_str());
            } else if (action == "clear") {
                ok = opts.login->clear(&err);
                std::fprintf(stderr, "[monitor] monitor login removed (edits only from this Linux again)\n");
            } else {
                err = "unknown_action";
            }
            if (body) cJSON_Delete(body);
            if (ok) {
                send_json(fd, 200, login_json(opts, peer, req));
            } else {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", err.c_str());
                send_json(fd, err == "local_browser_required" ? 403 : 400, e);
            }
        } else if (req.method == "POST" && path == "/api/control") {
            if (opts.systemd_unit.empty()) {
                cJSON* e = cJSON_CreateObject();
                cJSON_AddStringToObject(e, "error", "no systemd unit configured for this bridge");
                send_json(fd, 409, e);
            } else {
                cJSON* body = req.body.empty() ? nullptr : cJSON_Parse(req.body.c_str());
                cJSON* action_item = body ? cJSON_GetObjectItemCaseSensitive(body, "action") : nullptr;
                std::string action = cJSON_IsString(action_item) && action_item->valuestring
                                          ? action_item->valuestring
                                          : "";
                if (action == "stop" || action == "restart") {
                    std::fprintf(stderr, "[monitor] %s requested for unit %s\n", action.c_str(),
                                 opts.systemd_unit.c_str());
                    systemctl_detached(action, opts.systemd_unit);
                    cJSON* ok = cJSON_CreateObject();
                    cJSON_AddBoolToObject(ok, "ok", true);
                    cJSON_AddStringToObject(ok, "action", action.c_str());
                    send_json(fd, 200, ok);
                } else {
                    cJSON* e = cJSON_CreateObject();
                    cJSON_AddStringToObject(e, "error", "action must be \"stop\" or \"restart\"");
                    send_json(fd, 400, e);
                }
                if (body) cJSON_Delete(body);
            }
        } else {
            send_response(fd, 404, "not found", "text/plain");
        }
        close(fd);
    };

    for (;;) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) continue;
        // Bounded waits on every connection: a client that connects but
        // never finishes sending its request (or stops reading our reply)
        // ties up only its own thread, and only for this long.
        timeval tv{};
        tv.tv_sec = 5;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        try {
            std::thread(handle, fd, peer).detach();
        } catch (...) {
            close(fd);  // thread creation failed (resource limit) - drop just this one
        }
    }
}

}  // namespace monitor_server

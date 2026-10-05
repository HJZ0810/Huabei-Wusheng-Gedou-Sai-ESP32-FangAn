/**
 ******************************************************************************
 * @file    web_ui.h
 * @brief   单文件内嵌控制面板（HTML + CSS + JS 全部打包进 Flash）
 *
 * @details 设计取舍 —— 为什么要把整个网页塞进 PROGMEM：
 *            - 格斗车的使用场景是「赛场 / 野外 / 无外网」，必须做到**纯 AP 直连可用**；
 *              任何 CDN、字体外链、图标外链都会在断网时把页面变成白屏；
 *            - 不使用 SPIFFS / LittleFS：省掉一次分区烧录，避免用户忘记上传文件系统
 *              而得到「固件能跑但打不开页面」的经典故障；
 *            - 代价是固件体积增大约 47KB，对 16MB Flash 的 ESP32-S3 完全可忽略。
 *
 *          页面结构（四个 Tab）：
 *            - T1 驾驶：虚拟摇杆 + 键盘（WASD / 方向键）+ 速度滑杆 + 急停；
 *            - T2 精准：直线距离 / 转向角度输入 + 方向按钮组 + 舵机铲子；
 *            - T3 仪表：雷达图（六路红外）+ 姿态球（俯仰/横滚）+ 转速波形；
 *            - T4 设置：由 schema 驱动自动生成的表单，覆盖 Cfg 全部字段。
 *
 *          通信协议（与 web.cpp 严格对应，改字段名必须两边同步）：
 *            - 下行 t="tm"    遥测帧（15Hz）
 *            - 下行 t="done"  精准动作完成
 *            - 下行 t="cal"   标定流程（wait / ok / imu）
 *            - 下行 t="alert" 安全告警
 *            - 上行 t="drv"/"hb"/"stop"/"move"/"turn"/"servo"/"estop"/"unlock"/"cal"
 *
 * @author  HJZ
 * @version V1.1.0
 * @date    2026-10-05
 *
 * @par     修改记录
 *          <table>
 *          <tr><th>日期       <th>版本  <th>作者   <th>说明
 *          <tr><td>2026-10-04 <td>V1.0  <td>电控组 <td>首次创建，含纯 JS 二维码生成器
 *          <tr><td>2026-10-05 <td>V1.1  <td>电控组 <td>统一企业级注释规范
 *          </table>
 *
 * @note    本文件是 C++ 头文件，HTML 以 raw string（R"UIHTML(...)UIHTML"）承载，
 *          因此**正文里绝不能出现 ")UIHTML" 这个序列**，否则会提前截断。
 * @warning 修改完页面后必须重新编译烧录固件；页面不会从文件系统热加载。
 *
 * Copyright (c) 2026 HJZ. Licensed under the MIT License.
 ******************************************************************************
 */

#pragma once
#include <pgmspace.h>

/* ==========================================================================
 *                          内嵌页面（PROGMEM 常驻）
 * ========================================================================== */
static const char WEB_UI_HTML[] PROGMEM = R"UIHTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#0B0F14">
<title>CombatBot 控制台</title>
<style>
:root{
  --bg:#0B0F14; --card:#131A22; --line:rgba(255,255,255,.06);
  --cyan:#22D3EE; --orange:#FF8A3D; --ok:#34D399; --bad:#F43F5E;
  --txt:#E6EDF3; --sub:#8B98A5;
  --mono:ui-monospace,"JetBrains Mono",Consolas,Menlo,monospace;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;padding:0;background:var(--bg);color:var(--txt);
  font-family:system-ui,-apple-system,"Segoe UI",Inter,"Noto Sans SC",sans-serif;
  user-select:none;-webkit-user-select:none;touch-action:manipulation;
  overscroll-behavior:none}
body{padding-top:calc(56px + env(safe-area-inset-top));
     padding-bottom:calc(62px + env(safe-area-inset-bottom))}
.num{font-family:var(--mono);font-variant-numeric:tabular-nums}

/* ---------- 顶栏 ---------- */
header{position:fixed;top:0;left:0;right:0;height:calc(56px + env(safe-area-inset-top));
  padding:env(safe-area-inset-top) 10px 0;display:flex;align-items:center;gap:8px;
  background:rgba(19,26,34,.92);backdrop-filter:blur(10px);
  border-bottom:1px solid var(--line);z-index:50}
.badge{font-size:11px;padding:4px 8px;border-radius:8px;background:rgba(34,211,238,.12);
  color:var(--cyan);border:1px solid rgba(34,211,238,.35);font-family:var(--mono);white-space:nowrap}
.badge.sta{background:rgba(52,211,153,.12);color:var(--ok);border-color:rgba(52,211,153,.35)}
.badge.warn{background:rgba(244,63,94,.14);color:var(--bad);border-color:rgba(244,63,94,.4)}
.addr{flex:1;min-width:0;font-size:12px;color:var(--sub);font-family:var(--mono);
  overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.addr b{color:var(--txt);font-weight:600}
.iconbtn{width:34px;height:34px;border-radius:10px;border:1px solid var(--line);
  background:var(--card);color:var(--sub);display:flex;align-items:center;justify-content:center;font-size:15px}
.estop{flex:0 0 auto;height:36px;padding:0 14px;border-radius:10px;border:1px solid rgba(244,63,94,.5);
  background:rgba(244,63,94,.16);color:#FF8095;font-weight:700;font-size:13px;letter-spacing:.5px}
.estop.on{background:var(--bad);color:#fff;box-shadow:0 0 18px rgba(244,63,94,.55);animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.62}}

/* ---------- 布局 ---------- */
main{padding:12px;max-width:1100px;margin:0 auto}
.tab{display:none;animation:fade .18s ease}
.tab.on{display:block}
@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:12px}
.card h3{margin:0 0 10px;font-size:13px;color:var(--sub);font-weight:600;letter-spacing:.4px;
  display:flex;align-items:center;justify-content:space-between}
.row{display:flex;gap:12px;flex-wrap:wrap}
.row>*{flex:1;min-width:150px}
.grid2{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}

/* ---------- 连续控制 ---------- */
.joywrap{display:flex;justify-content:center;align-items:center;gap:16px;flex-wrap:wrap}
#joy{border-radius:50%;background:radial-gradient(circle at 50% 40%,#18222E,#0E141B);
  border:1px solid var(--line);touch-action:none;display:block}
.vslider{display:flex;flex-direction:column;align-items:center;gap:8px}
.vslider input[type=range]{writing-mode:vertical-lr;direction:rtl;height:150px;width:26px}
.val{font-family:var(--mono);font-size:20px;color:var(--cyan)}
.wheelrow{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.wheel{background:#0E141B;border:1px solid var(--line);border-radius:10px;padding:8px;text-align:center}
.wheel small{display:block;color:var(--sub);font-size:10px;margin-bottom:4px}
.wheel .num{font-size:14px;color:var(--cyan)}
.bar{height:6px;border-radius:3px;background:#0E141B;overflow:hidden;margin-top:6px}
.bar i{display:block;height:100%;background:linear-gradient(90deg,var(--cyan),var(--orange));width:0;transition:width .15s}
.big{font-family:var(--mono);font-size:26px;color:var(--cyan);text-align:center}
.sub{color:var(--sub);font-size:11px;text-align:center}

/* ---------- 精准控制 ---------- */
.inputcard{display:flex;align-items:center;gap:8px;background:#0E141B;border:1px solid var(--line);
  border-radius:10px;padding:8px 10px}
.inputcard label{font-size:12px;color:var(--sub);flex:0 0 auto}
.inputcard input{flex:1;min-width:0;width:100%;background:transparent;border:none;color:var(--txt);
  font-family:var(--mono);font-size:18px;text-align:right;outline:none}
.inputcard input.bad{border-bottom:1px solid var(--bad)}
.step{width:32px;height:32px;border-radius:8px;border:1px solid var(--line);background:var(--card);
  color:var(--txt);font-size:16px;line-height:1}
input[type=number]{-moz-appearance:textfield}
input[type=number]::-webkit-outer-spin-button,input[type=number]::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}
.chip{padding:6px 12px;border-radius:999px;border:1px solid var(--line);background:#0E141B;
  color:var(--sub);font-size:12px;font-family:var(--mono)}
.chip:active{transform:translateY(1px);background:var(--cyan);color:#06222B;border-color:var(--cyan)}
.dirgrid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:10px}
.btn{padding:12px 10px;border-radius:10px;border:1px solid var(--line);background:#0E141B;color:var(--txt);
  font-size:14px;transition:.15s}
.btn:active{transform:translateY(2px);background:rgba(34,211,238,.18);border-color:var(--cyan);box-shadow:0 0 14px rgba(34,211,238,.25)}
.btn.go{background:rgba(34,211,238,.14);border-color:rgba(34,211,238,.4);color:#BFF4FF}
.btn.warn{background:rgba(255,138,61,.14);border-color:rgba(255,138,61,.4);color:#FFD3AE}
.btn.danger{background:rgba(244,63,94,.14);border-color:rgba(244,63,94,.4);color:#FFB3C1}
.btn.wide{width:100%}
.prog{height:8px;border-radius:4px;background:#0E141B;overflow:hidden;margin:10px 0}
.prog i{display:block;height:100%;width:0;background:linear-gradient(90deg,var(--cyan),var(--ok));transition:width .12s}
.kv{display:flex;justify-content:space-between;font-family:var(--mono);font-size:13px;padding:3px 0}
.kv span:last-child{color:var(--cyan)}

/* ---------- 传感器 ---------- */
#radar,#ball,#wave{width:100%;display:block;border-radius:10px;background:#0E141B}
.lamps{display:flex;gap:10px;flex-wrap:wrap}
.lamp{flex:1;min-width:64px;text-align:center;background:#0E141B;border:1px solid var(--line);
  border-radius:10px;padding:10px 6px}
.dot{width:18px;height:18px;border-radius:50%;background:#22303D;margin:0 auto 6px;transition:.15s}
.dot.on{background:var(--ok);box-shadow:0 0 10px rgba(52,211,153,.7)}
.dot.hot{background:var(--bad);box-shadow:0 0 10px rgba(244,63,94,.7)}
.lamp small{color:var(--sub);font-size:10px;display:block}

/* ---------- 设置 ---------- */
.field{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:7px 0;
  border-bottom:1px dashed rgba(255,255,255,.04)}
.field:last-child{border-bottom:none}
.field label{font-size:12px;color:var(--sub);flex:1;min-width:0}
.field input[type=text],.field input[type=number],.field input[type=password]{
  width:120px;background:#0E141B;border:1px solid var(--line);border-radius:8px;color:var(--txt);
  font-family:var(--mono);font-size:13px;padding:6px 8px;text-align:right;outline:none}
.field input.bad{border-color:var(--bad)}
.field input[type=checkbox]{width:20px;height:20px;accent-color:var(--cyan)}
.unit{font-size:11px;color:var(--sub);font-family:var(--mono);margin-left:4px}
.hint{font-size:11px;color:var(--sub);line-height:1.6;margin:6px 0 0}
.btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
.btns .btn{flex:1;min-width:88px;font-size:13px;padding:10px 8px}

/* ---------- 底栏 ---------- */
nav{position:fixed;bottom:0;left:0;right:0;height:calc(60px + env(safe-area-inset-bottom));
  padding-bottom:env(safe-area-inset-bottom);display:flex;background:rgba(19,26,34,.95);
  backdrop-filter:blur(10px);border-top:1px solid var(--line);z-index:50}
nav button{flex:1;background:none;border:none;color:var(--sub);font-size:11px;display:flex;
  flex-direction:column;align-items:center;justify-content:center;gap:3px}
nav button i{font-style:normal;font-size:18px}
nav button.on{color:var(--cyan)}

/* ---------- toast / 弹窗 ---------- */
#toast{position:fixed;left:50%;transform:translateX(-50%);bottom:80px;z-index:99;
  display:flex;flex-direction:column;gap:6px;align-items:center;pointer-events:none}
.tst{background:rgba(19,26,34,.96);border:1px solid var(--line);border-radius:10px;padding:8px 14px;
  font-size:12px;animation:fade .18s ease}
.tst.ok{border-color:rgba(52,211,153,.5);color:var(--ok)}
.tst.bad{border-color:rgba(244,63,94,.5);color:var(--bad)}
.modal{position:fixed;inset:0;background:rgba(3,6,10,.86);display:none;align-items:center;
  justify-content:center;z-index:100;padding:20px}
.modal.on{display:flex}
.mbox{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px;
  text-align:center;max-width:320px;width:100%}
.mbox canvas{image-rendering:pixelated;background:#fff;border-radius:8px;padding:8px}
@media (min-width:760px) and (orientation:landscape){
  .row>.joywrap{flex:1.2}
}
</style>
</head>
<body>

<header>
  <span class="badge" id="net">AP</span>
  <span class="addr" id="addr">--</span>
  <span class="badge" id="bat">-- V</span>
  <span class="badge" id="rssi" style="display:none">--</span>
  <span class="badge" id="owner" style="display:none">被占用</span>
  <button class="iconbtn" id="qrbtn" title="二维码">▣</button>
  <button class="estop" id="estop">急停</button>
</header>

<main>
  <!-- ==================== 连续控制 ==================== -->
  <section class="tab on" id="t1">
    <div class="card">
      <h3>连续实时控制 <span style="color:var(--sub)">按住即走 · 松开即停</span></h3>
      <div class="joywrap">
        <canvas id="joy" width="240" height="240"></canvas>
        <div class="vslider">
          <div class="val num" id="spdv">60%</div>
          <input type="range" id="spd" min="0" max="100" value="60" orient="vertical">
          <div class="sub">速度上限</div>
          <div class="big num" id="yawv">0.0°</div>
          <div class="sub">航向角</div>
        </div>
      </div>
      <div class="hint">支持键盘 W/A/S/D 与方向键，空格=急停。摇杆死区 0.08，指数曲线 1.5。</div>
    </div>
    <div class="card">
      <h3>四轮转速 <span class="num" id="spdv2" style="color:var(--cyan)">0.0 cm/s</span></h3>
      <div class="wheelrow" id="wheels"></div>
    </div>
  </section>

  <!-- ==================== 精准控制 ==================== -->
  <section class="tab" id="t2">
    <div class="card">
      <h3>定点精准控制</h3>
      <div class="row">
        <div class="inputcard">
          <label>距离</label>
          <button class="step" data-step="-10" data-target="dist">−</button>
          <input type="number" id="dist" value="30" step="1" min="-500" max="500">
          <button class="step" data-step="10" data-target="dist">+</button>
          <span class="unit">cm</span>
        </div>
        <div class="inputcard">
          <label>角度</label>
          <button class="step" data-step="-15" data-target="deg">−</button>
          <input type="number" id="deg" value="90" step="5" min="-720" max="720">
          <button class="step" data-step="15" data-target="deg">+</button>
          <span class="unit">°</span>
        </div>
      </div>
      <div class="chips" id="chipsD"></div>
      <div class="chips" id="chipsA"></div>
      <div class="dirgrid">
        <button class="btn" data-act="tl">↺ 左转</button>
        <button class="btn go" data-act="fwd">↑ 前进</button>
        <button class="btn" data-act="tr">↻ 右转</button>
        <button class="btn warn" data-act="srv">舵机</button>
        <button class="btn" data-act="back">↓ 后退</button>
        <button class="btn danger" data-act="stopall">停止</button>
      </div>
      <div class="prog"><i id="pgbar"></i></div>
      <div class="kv"><span>目标</span><span id="rT">--</span></div>
      <div class="kv"><span>实际</span><span id="rA">--</span></div>
      <div class="kv"><span>误差</span><span id="rE">--</span></div>
      <div class="hint">左/右转为原地坦克转向（左右轮等速反向），采用梯形速度规划 + 三级闭环。</div>
    </div>
  </section>

  <!-- ==================== 传感器 ==================== -->
  <section class="tab" id="t3">
    <div class="card">
      <h3>红外测距雷达 <span class="num" id="irmin">-- cm</span></h3>
      <canvas id="radar" width="600" height="340"></canvas>
      <div class="hint">扇区：前 / 后 / 左前 / 右前 / 左后 / 右后；颜色越红越近。</div>
    </div>
    <div class="row">
      <div class="card">
        <h3>灰度 ×4</h3>
        <div class="lamps" id="gray"></div>
      </div>
      <div class="card">
        <h3>光电开关 ×3</h3>
        <div class="lamps" id="e18"></div>
      </div>
    </div>
    <div class="row">
      <div class="card">
        <h3>姿态小球</h3>
        <canvas id="ball" width="320" height="200"></canvas>
        <div class="hint">横轴=横滚 roll，纵轴=俯仰 pitch；小球偏离中心即车体倾斜。</div>
      </div>
      <div class="card">
        <h3>三轴加速度</h3>
        <canvas id="wave" width="320" height="200"></canvas>
        <div class="hint" id="accv">--</div>
      </div>
    </div>
  </section>

  <!-- ==================== 设置 ==================== -->
  <section class="tab" id="t4">
    <div class="card">
      <h3>标定（决定精度上限）</h3>
      <div class="field"><label>① IMU 零偏（静止放置后点击，采样 3 秒）</label>
        <button class="btn" id="calImu" style="width:110px">标定零偏</button></div>
      <div class="field"><label>② 行程：命令走 <span class="num" id="cald">100</span> cm</label>
        <button class="btn" id="calOdo" style="width:110px">开始行走</button></div>
      <div class="field"><label>实测距离</label>
        <input type="number" id="odoReal" step="0.1" placeholder="尺子量出的 cm"></div>
      <div class="field"><label></label>
        <button class="btn go" id="odoOk" style="width:110px">解算 K_odo</button></div>
      <div class="field"><label>③ 转向：命令转 <span class="num" id="calt">360</span>°</label>
        <button class="btn" id="calTurn" style="width:110px">开始转向</button></div>
      <div class="field"><label>实测角度</label>
        <input type="number" id="turnReal" step="1" placeholder="实际转过的度"></div>
      <div class="field"><label></label>
        <button class="btn go" id="turnOk" style="width:110px">解算 K_turn</button></div>
      <div class="hint">顺序：零偏 → 行程 → 转向。驼峰轮打滑严重，转向标定不可省略，可重复 1~2 次收敛。</div>
    </div>
    <div id="forms"></div>
    <div class="card">
      <h3>操作</h3>
      <div class="btns">
        <button class="btn go" id="save">保存配置</button>
        <button class="btn" id="reset">恢复默认</button>
        <button class="btn" id="exp">导出 JSON</button>
        <button class="btn" id="imp">导入 JSON</button>
        <button class="btn warn" id="reb">重启设备</button>
        <button class="btn danger" id="clr">清除 WiFi</button>
      </div>
      <input type="file" id="file" accept=".json" style="display:none">
      <div class="hint">保存到 NVS，掉电不丢；修改 AP/STA 后需重启设备生效。</div>
    </div>
  </section>
</main>

<nav>
  <button class="on" data-tab="t1"><i>◎</i>连续</button>
  <button data-tab="t2"><i>⌖</i>精准</button>
  <button data-tab="t3"><i>◈</i>传感器</button>
  <button data-tab="t4"><i>⚙</i>设置</button>
</nav>

<div id="toast"></div>
<div class="modal" id="qrm"><div class="mbox">
  <h3 style="margin:0 0 10px;font-size:14px;color:var(--sub)">扫码打开控制台</h3>
  <canvas id="qrc" width="240" height="240"></canvas>
  <div class="hint" id="qrtxt" style="word-break:break-all"></div>
  <button class="btn wide" id="qrclose" style="margin-top:12px">关闭</button>
</div></div>

<script>
/* =======================================================================
 * 0. 工具
 * ======================================================================= */
const $ = s => document.querySelector(s);
const $$ = s => Array.prototype.slice.call(document.querySelectorAll(s));
function toast(msg, kind){ const d=document.createElement('div');
  d.className='tst'+(kind?' '+kind:''); d.textContent=msg; $('#toast').appendChild(d);
  setTimeout(()=>d.remove(), 2600); }
function clamp(v,a,b){ return v<a?a:(v>b?b:v); }

/* =======================================================================
 * 1. WebSocket
 * ======================================================================= */
let ws=null, connected=false, retryT=null, myId=0;
const T = {rpm:[0,0,0,0],spd:0,yaw:0,bat:0,ir:[200,200,200,200,200,200],
           gray:[0,0,0,0],e18:[0,0,0],acc:[0,0,1],pg:0,st:0,es:0,lb:0,sv:1500,
           pit:0,rol:0,net:'AP',rssi:0,cal:0,imu:1,sl:0,own:0};
let lastSl=0;   /* 上一次的「传感器陈旧」标志，用于只提示一次 */
function send(o){ if(ws && ws.readyState===1) ws.send(JSON.stringify(o)); }
function connect(){
  clearTimeout(retryT);
  ws = new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');
  ws.onopen    = ()=>{ connected=true; $('#net').textContent=T.net; toast('已连接','ok'); };
  ws.onclose   = ()=>{ connected=false; stopAll(); $('#net').textContent='离线';
                       retryT=setTimeout(connect,1000); };
  ws.onerror   = ()=>{ try{ws.close()}catch(e){} };
  ws.onmessage = e => { let m; try{m=JSON.parse(e.data)}catch(err){return} onMsg(m); };
}
function onMsg(m){
  if(m.t==='tm'){
    T.rpm=m.rpm; T.spd=m.spd; T.yaw=m.yaw; T.bat=m.bat; T.ir=m.ir; T.gray=m.gr;
    T.e18=m.e18; T.acc=m.acc; T.pg=m.pg; T.st=m.st; T.es=m.es; T.lb=m.lb;
    T.sv=m.sv; T.pit=m.pit; T.rol=m.rol; T.net=m.net; T.rssi=m.rssi; T.cal=m.cal; T.imu=m.imu;
    T.sl=m.sl; T.own=(m.own===undefined?0:m.own);
    if(T.sl&&!lastSl) toast('传感器数据超时或 IO 扩展未连接，相关读数不可用','warn');
    lastSl=T.sl;
    paint();
  } else if(m.t==='done'){
    $('#rT').textContent = m.target.toFixed(2)+(m.type==='move'?' cm':' °');
    $('#rA').textContent = m.actual.toFixed(2)+(m.type==='move'?' cm':' °');
    $('#rE').textContent = (m.err>=0?'+':'')+m.err.toFixed(2);
    toast((m.type==='move'?'直线':'转向')+'完成，误差 '+m.err.toFixed(2),'ok');
  } else if(m.t==='cal'){
    if(m.wait){ $('#odoReal').value=''; $('#turnReal').value='';
      toast('动作完成，请量出实测值并填入','ok'); }
    else if(m.ok){ toast('标定完成：'+m.value.toFixed(4),'ok'); loadCfg(); }
    else if(m.mode==='imu'){ toast('零偏已写入：'+m.bias.toFixed(6),'ok'); loadCfg(); }
  } else if(m.t==='alert'){
    toast(m.msg,'bad');
  } else if(m.t==='hello'){
    /* 服务端在握手帧里带回本连接的编号；用于判断"车是不是被我占着" */
    if(m.id) myId=m.id;
  }
}

/* =======================================================================
 * 2. 连续控制：摇杆 + 键盘
 * ======================================================================= */
const DEAD=0.08, EXPO=1.5;
let spd=60, driving=false, hbTimer=null, drvT=0;
const stick={x:0,y:0};
const jc=$('#joy'), jx=jc.getContext('2d'), JR=jc.width/2-16;

function joyPaint(px,py){
  const c=jc.width/2;
  jx.clearRect(0,0,jc.width,jc.height);
  jx.strokeStyle='rgba(255,255,255,.07)'; jx.lineWidth=1;
  for(let r=40;r<=JR;r+=40){ jx.beginPath(); jx.arc(c,c,r,0,7); jx.stroke(); }
  jx.beginPath(); jx.moveTo(c,0); jx.lineTo(c,jc.height); jx.stroke();
  jx.beginPath(); jx.moveTo(0,c); jx.lineTo(jc.width,c); jx.stroke();
  const g=jx.createRadialGradient(c,c,2,c,c,26);
  g.addColorStop(0,'#FF8A3D'); g.addColorStop(1,'#22D3EE');
  jx.fillStyle=g; jx.beginPath(); jx.arc(c+px,c+py,26,0,7); jx.fill();
  jx.strokeStyle='rgba(255,255,255,.25)'; jx.lineWidth=2; jx.stroke();
}
joyPaint(0,0);

function joyFrom(ev){
  const r=jc.getBoundingClientRect();
  const cx=r.width/2, cy=r.height/2;
  let dx=ev.clientX-r.left-cx, dy=ev.clientY-r.top-cy;
  let m=Math.hypot(dx,dy);
  if(m>JR){ dx=dx*JR/m; dy=dy*JR/m; m=JR; }
  let nx=0, ny=0, mag=m/JR;
  if(mag>DEAD){ const s=Math.pow(mag,EXPO)/mag; nx=dx/JR*s; ny=-dy/JR*s; }
  stick.x=nx; stick.y=ny;
  joyPaint(dx*(r.width/jc.width), dy*(r.height/jc.height));
}
function startDrive(){ if(driving) return; driving=true; send({t:'drv',x:stick.x,y:stick.y,spd:spd});
  hbTimer=setInterval(()=>{ if(driving) send({t:'hb'}); },200); }
function stopAll(){ driving=false; clearInterval(hbTimer); stick.x=0; stick.y=0;
  joyPaint(0,0); send({t:'stop'}); }

jc.addEventListener('pointerdown',e=>{ jc.setPointerCapture(e.pointerId); joyFrom(e); startDrive(); });
jc.addEventListener('pointermove',e=>{ if(!driving) return; joyFrom(e);
  const n=Date.now(); if(n-drvT>60){ drvT=n; send({t:'drv',x:stick.x,y:stick.y,spd:spd}); } });
jc.addEventListener('pointerup',stopAll);
jc.addEventListener('pointercancel',stopAll);
jc.addEventListener('lostpointercapture',stopAll);
$('#spd').addEventListener('input',e=>{ spd=+e.target.value; $('#spdv').textContent=spd+'%'; });

const keys={};
function keyVec(){
  let x=0,y=0;
  if(keys['w']||keys['arrowup'])    y+=1;
  if(keys['s']||keys['arrowdown'])  y-=1;
  if(keys['a']||keys['arrowleft'])  x-=1;
  if(keys['d']||keys['arrowright']) x+=1;
  const m=Math.hypot(x,y); if(m>1){ x/=m; y/=m; }
  return [x,y];
}
window.addEventListener('keydown',e=>{
  const k=e.key.toLowerCase();
  if(['w','a','s','d','arrowup','arrowdown','arrowleft','arrowright'].indexOf(k)>=0){
    keys[k]=true; e.preventDefault();
    const v=keyVec(); stick.x=v[0]; stick.y=v[1]; startDrive();
    send({t:'drv',x:stick.x,y:stick.y,spd:spd});
  } else if(k===' '){ e.preventDefault(); $('#estop').click(); }
});
window.addEventListener('keyup',e=>{
  const k=e.key.toLowerCase(); delete keys[k];
  if(!Object.keys(keys).length) stopAll();
});
window.addEventListener('blur',stopAll);
document.addEventListener('visibilitychange',()=>{ if(document.hidden) stopAll(); });
window.addEventListener('beforeunload',()=>{ try{ ws.send(JSON.stringify({t:'stop'})) }catch(e){} });

/* =======================================================================
 * 3. 精准控制
 * ======================================================================= */
$$('.step').forEach(b=>b.addEventListener('click',()=>{
  const i=$('#'+b.dataset.target), s=parseFloat(i.value||0)+parseFloat(b.dataset.step);
  i.value=s.toFixed(0);
}));
[['chipsD',[10,30,50,100],'dist','cm'],['chipsA',[45,90,180,360],'deg','°']].forEach(cfg=>{
  const box=$('#'+cfg[0]);
  cfg[1].forEach(v=>{ const b=document.createElement('button'); b.className='chip';
    b.textContent=v+cfg[3]; b.onclick=()=>{ $('#'+cfg[2]).value=v; }; box.appendChild(b); });
});
$$('.dirgrid .btn').forEach(b=>b.addEventListener('click',()=>{
  const d=parseFloat($('#dist').value||0), a=parseFloat($('#deg').value||0);
  switch(b.dataset.act){
    case 'fwd':  send({t:'move',dist:Math.abs(d),spd:spd}); break;
    case 'back': send({t:'move',dist:-Math.abs(d),spd:spd}); break;
    case 'tl':   send({t:'turn',deg:Math.abs(a)}); break;
    case 'tr':   send({t:'turn',deg:-Math.abs(a)}); break;
    case 'stopall': send({t:'stop'}); break;
    case 'srv':  srvUp=!srvUp; send({t:'servo',pos:srvUp?1:0});
                 toast(srvUp?'举铲':'放铲'); break;
  }
}));
let srvUp=false;

/* =======================================================================
 * 4. 急停
 * ======================================================================= */
let locked=false;
$('#estop').addEventListener('click',()=>{
  locked=!locked;
  $('#estop').classList.toggle('on',locked);
  $('#estop').textContent = locked?'解锁':'急停';
  send({t:locked?'estop':'unlock'});
  if(locked){ stopAll(); toast('急停已锁定','bad'); } else toast('已解锁','ok');
});

/* =======================================================================
 * 5. 传感器可视化
 * ======================================================================= */
const wheelBox=$('#wheels');
['左前','右前','左后','右后'].forEach((n,i)=>{
  const d=document.createElement('div'); d.className='wheel';
  d.innerHTML='<small>'+n+'</small><div class="num">0</div><div class="bar"><i></i></div>';
  wheelBox.appendChild(d);
});
const grayBox=$('#gray'), e18Box=$('#e18');
for(let i=0;i<4;i++){ const d=document.createElement('div'); d.className='lamp';
  d.innerHTML='<div class="dot"></div><small>灰度'+(i+1)+'</small>'; grayBox.appendChild(d); }
for(let i=0;i<3;i++){ const d=document.createElement('div'); d.className='lamp';
  d.innerHTML='<div class="dot"></div><small>E18-'+(i+1)+'</small>'; e18Box.appendChild(d); }

const rc=$('#radar'), rx=rc.getContext('2d');
const IR_ANG=[0,180,45,-45,135,-135];     // 前/后/左前/右前/左后/右后（度，车体坐标）
const IR_NAME=['前','后','左前','右前','左后','右后'];
function drawRadar(){
  const W=rc.width,H=rc.height,cx=W/2,cy=H-26,R=Math.min(W/2-20,H-56);
  rx.clearRect(0,0,W,H);
  rx.fillStyle='#0E141B'; rx.fillRect(0,0,W,H);
  /* 距离圈 */
  rx.strokeStyle='rgba(255,255,255,.08)'; rx.fillStyle='rgba(255,255,255,.25)';
  rx.font='10px ui-monospace,monospace'; rx.textAlign='center';
  [30,60,90,120,150].forEach(d=>{
    const r=R*d/150;
    rx.beginPath(); rx.arc(cx,cy,r,Math.PI,2*Math.PI); rx.stroke();
    rx.fillText(d+'',cx+r-8,cy-4);
  });
  /* 扇区 */
  for(let i=0;i<6;i++){
    /* ★ 融合版：无效通道不画假的距离条，改为灰色扇区 + "--"，
       避免"传感器掉了"被误读成"障碍物贴脸"。 */
    const ok=(T.ir[i]!=null);
    const d=ok?clamp(T.ir[i],0,150):150, r=R*d/150;
    const a0=(-90-IR_ANG[i]-15)*Math.PI/180, a1=(-90-IR_ANG[i]+15)*Math.PI/180;
    const t=ok?clamp(1-d/150,0,1):0;
    const col=ok?('rgb('+Math.round(255*(1-t))+','+Math.round(211*t+60*(1-t))+','+Math.round(238*t+70*(1-t))+')')
                :'rgb(148,163,184)';
    rx.fillStyle=col.replace('rgb','rgba').replace(')',',0.28)');
    rx.beginPath(); rx.moveTo(cx,cy); rx.arc(cx,cy,R,a0,a1); rx.closePath(); rx.fill();
    rx.strokeStyle=col; rx.lineWidth=2;
    rx.beginPath(); rx.moveTo(cx,cy); rx.lineTo(cx+Math.cos(a0)*r,cy+Math.sin(a0)*r); rx.stroke();
    rx.beginPath(); rx.moveTo(cx,cy); rx.lineTo(cx+Math.cos(a1)*r,cy+Math.sin(a1)*r); rx.stroke();
    const am=(a0+a1)/2;
    rx.fillStyle=ok?'#E6EDF3':'#94A3B8'; rx.font='11px ui-monospace,monospace';
    rx.fillText(IR_NAME[i]+' '+(ok?d.toFixed(0):'--'),cx+Math.cos(am)*(R+14),cy+Math.sin(am)*(R+14));
  }
  /* 车体 */
  rx.fillStyle='#22D3EE';
  rx.beginPath(); rx.arc(cx,cy,7,0,7); rx.fill();
}
const bc=$('#ball'), bx=bc.getContext('2d');
function drawBall(){
  const W=bc.width,H=bc.height,cx=W/2,cy=H/2,R=Math.min(W,H)/2-14;
  bx.clearRect(0,0,W,H);
  bx.strokeStyle='rgba(255,255,255,.10)'; bx.lineWidth=1;
  bx.beginPath(); bx.arc(cx,cy,R,0,7); bx.stroke();
  bx.beginPath(); bx.arc(cx,cy,R*0.6,0,7); bx.stroke();
  bx.beginPath(); bx.moveTo(cx-R,cy); bx.lineTo(cx+R,cy); bx.stroke();
  bx.beginPath(); bx.moveTo(cx,cy-R); bx.lineTo(cx,cy+R); bx.stroke();
  const px=clamp(T.rol/45,-1,1)*R*0.85, py=clamp(T.pit/45,-1,1)*R*0.85;
  const g=bx.createRadialGradient(cx+px,cy+py,1,cx+px,cy+py,16);
  g.addColorStop(0,'#FF8A3D'); g.addColorStop(1,'#22D3EE');
  bx.fillStyle=g; bx.beginPath(); bx.arc(cx+px,cy+py,14,0,7); bx.fill();
  bx.fillStyle='#8B98A5'; bx.font='11px ui-monospace,monospace'; bx.textAlign='center';
  bx.fillText('roll '+T.rol.toFixed(1)+'°  pitch '+T.pit.toFixed(1)+'°',cx,H-8);
}
const wc=$('#wave'), wx=wc.getContext('2d');
const HIST=[[],[],[]];
function drawWave(){
  const W=wc.width,H=wc.height;
  wx.clearRect(0,0,W,H);
  wx.strokeStyle='rgba(255,255,255,.08)';
  for(let i=1;i<4;i++){ const y=H*i/4; wx.beginPath(); wx.moveTo(0,y); wx.lineTo(W,y); wx.stroke(); }
  const cols=['#22D3EE','#FF8A3D','#34D399'];
  for(let a=0;a<3;a++){
    const h=HIST[a]; if(h.length<2) continue;
    wx.strokeStyle=cols[a]; wx.lineWidth=1.6; wx.beginPath();
    for(let i=0;i<h.length;i++){
      const x=W*i/(HIST[0].length-1||1), y=H/2-clamp(h[i],-3,3)*(H/2-10)/3;
      i?wx.lineTo(x,y):wx.moveTo(x,y);
    }
    wx.stroke();
  }
  $('#accv').textContent='X '+T.acc[0].toFixed(2)+'  Y '+T.acc[1].toFixed(2)+'  Z '+T.acc[2].toFixed(2)+' (g)';
}

/* =======================================================================
 * 6. 状态刷新
 * ======================================================================= */
let lastPaint=0;
function paint(){
  const now=Date.now();
  $('#net').textContent = connected?T.net:'离线';
  $('#net').className = 'badge'+(T.net==='AP+STA'?' sta':'');
  $('#addr').innerHTML = '<b>'+location.host+'</b>';
    const b=$('#bat');
    /* ★ 融合版：电池分压未接 / 读数无效时显示 "--"，不要编一个电压出来 */
    b.textContent=(T.bat==null?'--':T.bat.toFixed(1)+' V');
    b.className='badge'+(T.lb?' warn':'');
  const r=$('#rssi');
  if(T.net==='AP+STA'){ r.style.display=''; r.textContent=T.rssi+' dBm'; } else r.style.display='none';
  /* 控制权提示：车已被别的终端接管时给出明确反馈，避免"点了没反应" */
  const ow=$('#owner');
  if(T.own && myId && T.own!==myId){
    ow.style.display=''; ow.className='badge warn';
    ow.textContent = (T.own===0xFFFFFFFF)?'REST 占用':'被占用('+T.own+')';
  } else ow.style.display='none';
  $('#yawv').textContent=T.yaw.toFixed(1)+'°';
  $('#spdv2').textContent=T.spd.toFixed(1)+' cm/s';
  $$('#wheels .wheel').forEach((w,i)=>{
    w.querySelector('.num').textContent=T.rpm[i].toFixed(0)+' rpm';
    w.querySelector('.bar i').style.width=clamp(Math.abs(T.rpm[i])/600*100,0,100)+'%';
  });
  $('#pgbar').style.width=(T.pg*100).toFixed(1)+'%';
  /* 只对"有效"的通道取最小值；一路都没接时显示 "--" */
  const irv=T.ir.filter(v=>v!=null);
  $('#irmin').textContent = irv.length ? Math.min.apply(null,irv).toFixed(0)+' cm' : '--';
  $$('#gray .dot').forEach((d,i)=>{ d.className='dot'+(T.gray[i]?' on':''); });
  $$('#e18 .dot').forEach((d,i)=>{ d.className='dot'+(T.e18[i]?' hot':''); });
  if(now-lastPaint>60){
    lastPaint=now;
    for(let a=0;a<3;a++){ HIST[a].push(T.acc[a]); if(HIST[a].length>100) HIST[a].shift(); }
    if($('#t3').classList.contains('on')){ drawRadar(); drawBall(); drawWave(); }
  }
  if($('#t4').classList.contains('on')&&!formsBuilt) buildForms();
}

/* =======================================================================
 * 7. 设置表单（schema 驱动）
 * ======================================================================= */
const GROUPS=[
 ['网络',[
  ['apSsid','AP 名称','text'],['apPass','AP 密码','text'],
  ['staSsid','路由器 SSID','text'],['staPass','路由器密码','password'],
  ['staEnable','启用 STA 联网','bool'],['mdns','mDNS 主机名','text'],
  ['captive','强制门户(自动弹页)','bool']]],
 ['机械',[
  ['wheelDia','轮径 D','num','mm',20,200,0.1],['wheelBase','轴距 L','num','mm',50,400,1],
  ['track','轮距 T','num','mm',50,400,1],['ppr','FG 每转脉冲 PPR','num','',1,2000,1]]],
 ['标定系数',[
  ['kOdo','K_odo 脉冲/厘米','num','/cm',0.05,100,0.001],
  ['kTurn','K_turn 转向打滑系数','num','',0.4,2.5,0.001],
  ['gzBias','陀螺 Z 零偏','num','rad/s',-1,1,0.000001],
  ['gyroInvert','陀螺 Z 轴取反','bool'],
  ['calDist','行程标定命令距离','num','cm',10,500,1],
  ['calTurnDeg','转向标定命令角度','num','°',90,720,1]]],
 ['运动',[
  ['maxSpeed','最大速度','num','cm/s',5,220,1],['defSpeed','默认速度','num','cm/s',1,220,1],
  ['accel','加速度上限','num','cm/s²',5,400,1],['decel','减速度上限','num','cm/s²',5,400,1],
  ['maxOmega','转向最大角速度','num','°/s',10,600,1],
  ['turnAccel','转向角加速度','num','°/s²',10,2000,10],
  ['turnDecel','转向角减速度','num','°/s²',10,2000,10]]],
 ['PID',[
  ['vKp','速度环 Kp','num','',0,2,0.001],['vKi','速度环 Ki','num','',0,5,0.001],['vKd','速度环 Kd','num','',0,1,0.0001],
  ['pKp','位置环 Kp','num','',0,5,0.001],['pKi','位置环 Ki','num','',0,5,0.001],['pKd','位置环 Kd','num','',0,5,0.001],
  ['hKp','航向环 Kp','num','',0,10,0.01],['hKi','航向环 Ki','num','',0,10,0.01],['hKd','航向环 Kd','num','',0,10,0.01],
  ['alpha','融合系数 α(IMU权重)','num','',0,1,0.01]]],
 ['电机',[
  ['pwmDead','PWM 死区','num','',0,400,1],['pwmMax','PWM 上限','num','',100,1023,1],
  ['balLR','左右轮速差补偿','num','',-0.5,0.5,0.01],
  ['scaleL','左侧出力缩放','num','',0.5,1.5,0.01],['scaleR','右侧出力缩放','num','',0.5,1.5,0.01]]],
 ['电机极性',[
  ['invert0','左前 反转','bool'],['invert1','右前 反转','bool'],
  ['invert2','左后 反转','bool'],['invert3','右后 反转','bool']]],
 ['传感器',[
  ['grayInvert','灰度逻辑取反','bool'],['irAlarm','红外报警距离','num','cm',5,150,1],
  ['irScale','红外测距比例修正','num','',0.5,2,0.01],
  ['e18ActiveLow','E18 低电平触发','bool'],
  ['safetyEdge','边缘保护(E18刹车)','bool'],['safetyIR','红外接近限速','bool'],
  ['impactThresh','撞击检测阈值','num','g',0.5,8,0.1]]],
 ['舵机',[
  ['servoCenter','中位脉宽','num','us',500,2500,1],['servoMin','最小脉宽','num','us',500,2500,1],
  ['servoMax','最大脉宽','num','us',500,2500,1],
  ['servoUp','举铲脉宽','num','us',500,2500,1],['servoDown','放铲脉宽','num','us',500,2500,1]]],
 ['安全',[
  ['hbTimeout','心跳超时','num','ms',300,10000,100],
  ['batLow','低压告警','num','V',15,30,0.1],['batCrit','严重低压停车','num','V',15,30,0.1],
  ['stallTime','堵转判定时间','num','ms',300,5000,100],
  ['lostBrake','失联时刹车(否则惰行)','bool']]],
 ['系统',[['telemetryHz','遥测推送频率','num','Hz',5,30,1]]]
];
let formsBuilt=false, CFG={};
function buildForms(){
  const box=$('#forms'); box.innerHTML='';
  GROUPS.forEach(g=>{
    const c=document.createElement('div'); c.className='card';
    let h='<h3>'+g[0]+'</h3>';
    g[1].forEach(f=>{
      const k=f[0], l=f[1], t=f[2];
      if(t==='bool'){
        h+='<div class="field"><label>'+l+'</label><input type="checkbox" data-k="'+k+'"></div>';
      } else {
        const min=f[4]!==undefined?f[4]:'', max=f[5]!==undefined?f[5]:'', st=f[6]!==undefined?f[6]:'1';
        h+='<div class="field"><label>'+l+'</label><input type="'+(t==='num'?'number':'text')+
           '" data-k="'+k+'" min="'+min+'" max="'+max+'" step="'+st+'"><span class="unit">'+(f[3]||'')+'</span></div>';
      }
    });
    c.innerHTML=h; box.appendChild(c);
  });
  formsBuilt=true;
}
function fillForms(){
  if(!formsBuilt) return;
  $$('#forms input[data-k]').forEach(i=>{
    const k=i.dataset.k;
    let v;
    if(k.indexOf('invert')===0) v=!!(CFG.invert&&CFG.invert[+k.slice(6)]);
    else v=CFG[k];
    if(i.type==='checkbox') i.checked=!!v; else i.value=(v===undefined?'':v);
  });
  $('#cald').textContent=CFG.calDist!==undefined?CFG.calDist:100;
  $('#calt').textContent=CFG.calTurnDeg!==undefined?CFG.calTurnDeg:360;
}
function collectForms(){
  const o={}; let bad=false;
  $$('#forms input[data-k]').forEach(i=>{
    const k=i.dataset.k;
    if(i.type==='checkbox'){
      const v=i.checked;
      if(k.indexOf('invert')===0){ if(!o.invert) o.invert=[false,false,false,false];
        o.invert[+k.slice(6)]=v; }
      else o[k]=v;
      return;
    }
    let v=i.value;
    if(i.type==='number'){
      v=parseFloat(v);
      const mn=parseFloat(i.min), mx=parseFloat(i.max);
      if(isNaN(v)||(!isNaN(mn)&&v<mn)||(!isNaN(mx)&&v>mx)){ i.classList.add('bad'); bad=true; return; }
      i.classList.remove('bad');
    }
    if(k.indexOf('invert')===0) return;
    o[k]=v;
  });
  return bad?null:o;
}
function loadCfg(){ fetch('/api/config').then(r=>r.json()).then(j=>{ CFG=j; fillForms(); }); }

$('#save').onclick=()=>{ const o=collectForms(); if(!o){ toast('存在非法值，已红框标出','bad'); return; }
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})
    .then(()=>{ toast('已保存到 NVS','ok'); loadCfg(); }); };
$('#reset').onclick=()=>{ if(!confirm('恢复出厂默认？')) return;
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({__reset:1})}).then(()=>{ toast('已恢复默认','ok'); loadCfg(); }); };
$('#exp').onclick=()=>{ const b=new Blob([JSON.stringify(CFG,null,2)],{type:'application/json'});
  const a=document.createElement('a'); a.href=URL.createObjectURL(b);
  a.download='combatbot-config.json'; a.click(); };
$('#imp').onclick=()=>$('#file').click();
$('#file').onchange=e=>{ const f=e.target.files[0]; if(!f) return;
  const r=new FileReader(); r.onload=()=>{ let j; try{j=JSON.parse(r.result)}catch(err){toast('JSON 解析失败','bad');return}
    fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(j)})
      .then(()=>{ toast('导入成功','ok'); loadCfg(); }); };
  r.readAsText(f); };
$('#reb').onclick=()=>{ if(!confirm('确定重启设备？')) return;
  fetch('/api/reboot',{method:'POST'}).then(()=>toast('重启中…')); };
$('#clr').onclick=()=>{ if(!confirm('清除 WiFi 配置？AP 将恢复默认名称/密码')) return;
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({staEnable:false,staSsid:'',staPass:'',apSsid:'CombatBot-AP',apPass:'12345678'})})
    .then(()=>{ toast('已清除，重启后生效','ok'); loadCfg(); }); };

/* --- 标定 --- */
$('#calImu').onclick =()=>{ send({t:'cal',mode:'imu'}); toast('采样中，请勿移动车身…'); };
$('#calOdo').onclick =()=>{ send({t:'cal',mode:'odo'}); toast('开始行程标定…'); };
$('#calTurn').onclick=()=>{ send({t:'cal',mode:'turn'}); toast('开始转向标定…'); };
$('#odoOk').onclick  =()=>{ const v=parseFloat($('#odoReal').value);
  if(!v||v<1){ toast('请输入实测距离','bad'); return; } send({t:'cal',mode:'odo',actual:v}); };
$('#turnOk').onclick =()=>{ const v=parseFloat($('#turnReal').value);
  if(!v||Math.abs(v)<1){ toast('请输入实测角度','bad'); return; } send({t:'cal',mode:'turn',actual:v}); };

/* =======================================================================
 * 8. Tab 切换
 * ======================================================================= */
$$('nav button').forEach(b=>b.addEventListener('click',()=>{
  $$('nav button').forEach(x=>x.classList.remove('on')); b.classList.add('on');
  $$('.tab').forEach(s=>s.classList.remove('on')); $('#'+b.dataset.tab).classList.add('on');
  if(b.dataset.tab==='t4'&&!formsBuilt){ buildForms(); fillForms(); }
  if(b.dataset.tab==='t3'){ drawRadar(); drawBall(); drawWave(); }
}));

/* =======================================================================
 * 9. 二维码（纯前端生成，无外部依赖）
 * ======================================================================= */
const QR=(function(){
  const EXP=new Uint8Array(512), LOG=new Uint8Array(256);
  (function(){ let x=1; for(let i=0;i<255;i++){ EXP[i]=x; LOG[x]=i; x<<=1; if(x&0x100) x^=0x11D; }
    for(let i=255;i<512;i++) EXP[i]=EXP[i-255]; })();
  function mul(a,b){ return (a&&b)?EXP[LOG[a]+LOG[b]]:0; }
  function genPoly(n){ let g=[1];
    for(let i=0;i<n;i++){ const ng=new Array(g.length+1).fill(0);
      for(let j=0;j<g.length;j++){ ng[j]^=mul(g[j],EXP[i]); ng[j+1]^=g[j]; } g=ng; }
    return g.reverse(); }                       /* 最高次在前 */
  function ecc(data,n){ const g=genPoly(n), res=new Array(data.length+n).fill(0);
    for(let i=0;i<data.length;i++) res[i]=data[i];
    for(let i=0;i<data.length;i++){ const f=res[i];
      if(f) for(let j=0;j<=n;j++) res[i+j]^=mul(g[j],f); }
    return res.slice(data.length); }
  const CAP=[null,
    {t:26,e:7,b:[[1,19]]},{t:44,e:10,b:[[1,34]]},{t:70,e:15,b:[[1,55]]},
    {t:100,e:20,b:[[1,80]]},{t:134,e:26,b:[[1,108]]},{t:172,e:18,b:[[2,68]]},
    {t:196,e:20,b:[[2,78]]},{t:242,e:24,b:[[2,97]]},{t:292,e:30,b:[[2,116]]},
    {t:346,e:18,b:[[2,68],[2,69]]}];
  const ALIGN={1:[],2:[6,18],3:[6,22],4:[6,26],5:[6,30],6:[6,34],7:[6,22,38],
               8:[6,24,42],9:[6,26,46],10:[6,28,50]};
  function utf8(s){ const o=[]; for(const ch of s){ let c=ch.codePointAt(0);
      if(c<0x80) o.push(c); else if(c<0x800){ o.push(0xC0|c>>6,0x80|c&63); }
      else if(c<0x10000){ o.push(0xE0|c>>12,0x80|(c>>6)&63,0x80|c&63); }
      else { o.push(0xF0|c>>18,0x80|(c>>12)&63,0x80|(c>>6)&63,0x80|c&63); } }
    return o; }
  function bchFormat(d){ let r=d;                    /* BCH(15,5)，生成多项式 0x537 */
    for(let i=0;i<10;i++) r=(r<<1)^(((r>>>9)&1)*0x537);
    return ((d<<10)|(r&0x3FF))^0x5412; }
  function bchVersion(v){ let r=v;                   /* BCH(18,6)，生成多项式 0x1F25 */
    for(let i=0;i<12;i++) r=(r<<1)^(((r>>>11)&1)*0x1F25);
    return (v<<12)|(r&0x3FFFF); }
  function maskAt(m,x,y){ switch(m){
    case 0:return (x+y)%2===0; case 1:return y%2===0; case 2:return x%3===0;
    case 3:return (x+y)%3===0; case 4:return ((y>>1)+(x/3|0))%2===0;
    case 5:return (x*y)%2+(x*y)%3===0; case 6:return ((x*y)%2+(x*y)%3)%2===0;
    default:return ((x+y)%2+(x*y)%3)%2===0; } }
  function penalty(m,size,func){
    let p=0;
    for(let i=0;i<size;i++){ let run=1,prev=-1;
      for(let j=0;j<size;j++){ const v=(m[i][j]?1:0); if(v===prev) run++; else { if(run>=5) p+=3+(run-5); run=1; } prev=v; }
      if(run>=5) p+=3+(run-5); }
    for(let j=0;j<size;j++){ let run=1,prev=-1;
      for(let i=0;i<size;i++){ const v=(m[i][j]?1:0); if(v===prev) run++; else { if(run>=5) p+=3+(run-5); run=1; } prev=v; }
      if(run>=5) p+=3+(run-5); }
    for(let i=0;i<size-1;i++) for(let j=0;j<size-1;j++){
      const v=m[i][j]; if(v===m[i][j+1]&&v===m[i+1][j]&&v===m[i+1][j+1]) p+=3; }
    let dark=0;
    for(let i=0;i<size;i++) for(let j=0;j<size;j++) if(m[i][j]) dark++;
    p+=10*Math.floor(Math.abs(dark*100/(size*size)-50)/5);
    return p; }
  function make(text){
    const bytes=utf8(text);
    let ver=0, cap=null;
    for(let v=1;v<=10;v++){ let dt=0; CAP[v].b.forEach(b=>dt+=b[0]*b[1]);
      if(4+8+8*bytes.length<=dt*8){ ver=v; cap=CAP[v]; break; } }
    if(!ver) return null;
    const size=ver*4+17;
    /* ---- 数据码字 ---- */
    let dc=0; cap.b.forEach(b=>dc+=b[0]*b[1]);
    const bits=[];
    const push=(val,n)=>{ for(let i=n-1;i>=0;i--) bits.push((val>>i)&1); };
    push(4,4); push(bytes.length,8); bytes.forEach(b=>push(b,8));
    for(let i=0;i<4&&bits.length<dc*8;i++) bits.push(0);
    while(bits.length%8) bits.push(0);
    const data=[]; for(let i=0;i<bits.length;i+=8){ let b=0; for(let j=0;j<8;j++) b=(b<<1)|bits[i+j]; data.push(b); }
    let pad=0xEC; while(data.length<dc){ data.push(pad); pad=pad===0xEC?0x11:0xEC; }
    /* ---- 分块 + 交织 ---- */
    const blocks=[]; let off=0;
    cap.b.forEach(spec=>{ for(let i=0;i<spec[0];i++){ const d=data.slice(off,off+spec[1]); off+=spec[1];
      blocks.push({d:d,e:ecc(d,cap.e)}); } });
    const final=[]; const maxD=Math.max.apply(null,blocks.map(b=>b.d.length));
    for(let i=0;i<maxD;i++) blocks.forEach(b=>{ if(i<b.d.length) final.push(b.d[i]); });
    for(let i=0;i<cap.e;i++) blocks.forEach(b=>final.push(b.e[i]));
    const fbits=[]; final.forEach(b=>{ for(let i=7;i>=0;i--) fbits.push((b>>i)&1); });
    /* ---- 功能图形 ---- */
    const mod=[],func=[];
    for(let i=0;i<size;i++){ mod.push(new Array(size).fill(0)); func.push(new Array(size).fill(false)); }
    const finder=(r,c)=>{ for(let i=-1;i<8;i++) for(let j=-1;j<8;j++){
        const y=r+i,x=c+j; if(y<0||y>=size||x<0||x>=size) continue;
        func[y][x]=true;
        const d=Math.max(Math.abs(i-3),Math.abs(j-3));
        mod[y][x]=(d!==2&&d<=3)?1:0; } };
    finder(0,0); finder(0,size-7); finder(size-7,0);
    for(let i=8;i<size-8;i++){ func[6][i]=func[i][6]=true; mod[6][i]=mod[i][6]=(i%2===0)?1:0; }
    const ap=ALIGN[ver];
    for(const r of ap) for(const c of ap){
      if((r<8&&c<8)||(r<8&&c>size-9)||(r>size-9&&c<8)) continue;
      for(let i=-2;i<=2;i++) for(let j=-2;j<=2;j++){ func[r+i][c+j]=true;
        mod[r+i][c+j]=(Math.max(Math.abs(i),Math.abs(j))!==1)?1:0; } }
    for(let i=0;i<9;i++){ if(i!==6){ func[8][i]=true; func[i][8]=true; } }
    func[size-8][8]=true; func[8][size-8]=true;
    for(let i=0;i<8;i++){ func[8][size-1-i]=true; func[size-1-i][8]=true; }
    /* ---- 数据填充 ---- */
    let bi=0;
    for(let right=size-1;right>=1;right-=2){
      if(right===6) right=5;
      for(let vert=0;vert<size;vert++){
        for(let j=0;j<2;j++){
          const x=right-j, upward=((right+1)&2)===0, y=upward?size-1-vert:vert;
          if(!func[y][x]&&bi<fbits.length){ mod[y][x]=fbits[bi++]?1:0; func[y][x]=true; }
        } } }
    /* ---- 掩码择优 ---- */
    let best=null,bestP=1e18,bestM=0;
    for(let m=0;m<8;m++){
      const t=mod.map(r=>r.slice());
      for(let y=0;y<size;y++) for(let x=0;x<size;x++) if(!func[y][x]&&maskAt(m,x,y)) t[y][x]^=1;
      const p=penalty(t,size,func);
      if(p<bestP){ bestP=p; best=t; bestM=m; }
    }
    /* ---- 格式 / 版本信息（ECC level L = 0b01） ---- */
    const fmt=bchFormat((1<<3)|bestM), fb=i=>(fmt>>i)&1;
    for(let i=0;i<=5;i++) best[i][8]=fb(i);          /* 第一份：第 8 列 */
    best[7][8]=fb(6); best[8][8]=fb(7); best[8][7]=fb(8);
    for(let i=9;i<15;i++) best[8][14-i]=fb(i);       /* 第一份：第 8 行 */
    for(let i=0;i<8;i++) best[8][size-1-i]=fb(i);    /* 第二份：右侧第 8 行 */
    for(let i=8;i<15;i++) best[size-15+i][8]=fb(i);  /* 第二份：下方第 8 列 */
    best[size-8][8]=1;                               /* 固定黑模块 */
    if(ver>=7){ const vi=bchVersion(ver);
      for(let i=0;i<18;i++){ const b=(vi>>i)&1;
        const r=(i/3)|0, c=i%3;
        best[r][size-11+c]=b; best[size-11+c][r]=b; } }
    return {size:size,mod:best};
  }
  return {make:make};
})();
function showQR(text){
  const r=QR.make(text); const cv=$('#qrc');
  if(!r){ $('#qrtxt').textContent=text; $('#qrm').classList.add('on'); return; }
  const n=r.size, s=Math.floor(240/n)||1, pad=2;
  cv.width=cv.height=(n+pad*2)*s;
  const c=cv.getContext('2d');
  c.fillStyle='#fff'; c.fillRect(0,0,cv.width,cv.height);
  c.fillStyle='#000';
  for(let y=0;y<n;y++) for(let x=0;x<n;x++) if(r.mod[y][x]) c.fillRect((x+pad)*s,(y+pad)*s,s,s);
  $('#qrtxt').textContent=text; $('#qrm').classList.add('on');
}
$('#qrbtn').onclick=()=>showQR(location.protocol+'//'+location.host+'/');
$('#qrclose').onclick=()=>$('#qrm').classList.remove('on');
$('#qrm').onclick=e=>{ if(e.target.id==='qrm') $('#qrm').classList.remove('on'); };

/* =======================================================================
 * 10. 启动
 * ======================================================================= */
fetch('/api/status').then(r=>r.json()).then(s=>{
  $('#addr').innerHTML='<b>'+location.host+'</b> · '+s.mdns;
}).catch(()=>{});
connect();
loadCfg();
setInterval(()=>{ if(connected) send({t:'hb'}); }, 3000);   /* 保活（非驾驶状态） */
drawRadar(); drawBall(); drawWave();
</script>
</body>
</html>
)UIHTML";

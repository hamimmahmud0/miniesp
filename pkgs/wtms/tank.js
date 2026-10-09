// Shared helpers for the tank pages: login token, CGI calls, force-control widget.
const $=i=>document.getElementById(i);
const T={
  tok:()=>{try{return localStorage.getItem('tank_tok')||''}catch(e){return''}},
  setTok:t=>{try{t?localStorage.setItem('tank_tok',t):localStorage.removeItem('tank_tok')}catch(e){}},
  hex:s=>Array.from(new TextEncoder().encode(s)).map(b=>b.toString(16).padStart(2,'0')).join(''),
  async raw(args,tries=6){
    for(let i=0;i<tries;i++){
      try{const r=await fetch('/cgi-bin/wtms?'+args.join('+'),{cache:'no-store'});
        if(r.status!==503){const t=await r.text();try{return JSON.parse(t)}catch(e){return{ok:false,error:'bad reply'}}}}catch(e){}
      await new Promise(r=>setTimeout(r,1500));
    }
    throw new Error('Board busy or unreachable');
  },
  async call(sub,...args){                       // authenticated call; a rejected token sends you to the login page
    const r=await T.raw(['web',sub,T.tok(),...args]);
    if(r.error==='auth'){T.setTok('');if(!T.noRedirect)location.href='login.html?next='+encodeURIComponent(location.pathname.split('/').pop()||'index.html')}
    return r;
  },
  async loggedIn(){ if(!T.tok())return false; try{const r=await T.raw(['web','me',T.tok()],2);return !!r.ok}catch(e){return false} },
  async logout(){ try{await T.raw(['web','logout',T.tok()],1)}catch(e){} T.setTok(''); location.href='login.html' },
  fmtLeft:s=>s>=3600?Math.floor(s/3600)+' h '+Math.floor(s%3600/60)+' min':s>=60?Math.floor(s/60)+' min':s+' s',
  // Force-control widget (needs a logged-in user). Renders into element `el`.
  mountControls(el,onChange){
    el.innerHTML=`<div class="banner" id="fb">Automatic</div>
     <div class="row"><label for="fdur">Force ON for</label><select id="fdur"><option value="15">15 minutes</option><option value="30" selected>30 minutes</option><option value="60">1 hour</option><option value="120">2 hours</option><option value="360">6 hours</option></select></div>
     <div class="btns"><button class="b ok" id="fon">Force pump ON</button><button class="b bad" id="foff">Force pump OFF</button>
       <button class="b sec" id="frel">Release &rarr; auto</button><button class="b sec" id="fclr">Clear fault</button><button class="b sec" id="fauto">Auto: --</button></div>
     <div class="msg" id="fmsg"></div>
     <div class="sub"><b>Force ON</b> runs the pump regardless of level, auto mode and faults until the time is up (an overflow guard still stops it at 100%). <b>Force OFF</b> keeps it off until you release it. The pump relay's auto-off timer remains the last line of defence.</div>`;
    const msg=(t,c)=>{$('fmsg').textContent=t;$('fmsg').className='msg '+(c||'')};
    const run=async(cmd,ok)=>{ for(const b of el.querySelectorAll('button'))b.disabled=true; msg('Sending…');
      try{const s=await T.call('act',cmd); if(s.ts!==undefined){msg(ok,'g');T.showForce(s);onChange&&onChange(s)}else if(s.error!=='auth')msg('Failed','e')}catch(e){msg(e.message,'e')}
      for(const b of el.querySelectorAll('button'))b.disabled=false; };
    $('fon').onclick=()=>{ if(confirm('Force the pump ON for '+$('fdur').value+' minutes, ignoring level and faults?'))run('force_on:'+$('fdur').value,'Pump forced ON') };
    $('foff').onclick=()=>{ if(confirm('Force the pump OFF until you release it? Automatic filling will not run.'))run('force_off','Pump forced OFF') };
    $('frel').onclick=()=>run('release','Back to automatic control');
    $('fclr').onclick=()=>run('reset','Fault cleared');
    $('fauto').onclick=async()=>{ const on=$('fauto').dataset.auto!=='ON'; for(const b of el.querySelectorAll('button'))b.disabled=true;
      try{const s=await T.call('auto',on?'on':'off'); if(s.ts!==undefined){T.showForce(s);msg('Auto '+(on?'enabled':'disabled'),'g');onChange&&onChange(s)}}catch(e){msg(e.message,'e')}
      for(const b of el.querySelectorAll('button'))b.disabled=false; };
  },
  showForce(s){
    const b=$('fb'); if(!b)return;
    if(s.force==='on'){b.className='banner on';b.textContent='FORCED ON'+(s.force_left?' — '+T.fmtLeft(s.force_left)+' left':'')}
    else if(s.force==='off'){b.className='banner off';b.textContent='FORCED OFF — until released'+(s.force_left?' ('+T.fmtLeft(s.force_left)+' left)':'')}
    else{b.className='banner';b.textContent=s.auto==='ON'?'Automatic: pump follows the level':'Automatic fill is OFF'}
    const a=$('fauto'); if(a){a.dataset.auto=s.auto;a.textContent='Auto fill: '+s.auto}
  }
};

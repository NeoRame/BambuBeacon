(function () {
  const c = document.getElementById("particleCanvas");
  if (!c) return;
  const ctx = c.getContext("2d");

  function resize() {
    c.width = window.innerWidth;
    c.height = window.innerHeight;
  }
  window.addEventListener("resize", resize);
  resize();

  function rnd(a, b) { return a + Math.random() * (b - a); }

  const pulses = [];
  const sparks = [];
  const filaments = [];
  const pulseGapMs = 2200;
  let lastPulse = 0;

  function spawnPulse(now) {
    pulses.push({
      t0: now,
      r0: 10,
      speed: rnd(0.015, 0.03),
      alpha: rnd(0.10, 0.18)
    });
  }

  for (let i = 0; i < 10; i++) {
    filaments.push({
      x: rnd(0, 1),
      y: rnd(0, 1),
      w: rnd(40, 120),
      drift: rnd(0.0001, 0.0003)
    });
  }

  for (let i = 0; i < 8; i++) {
    sparks.push({
      ang: rnd(0, Math.PI * 2),
      r: rnd(60, 240),
      vr: rnd(0.02, 0.08),
      size: rnd(1.0, 2.0),
      tw: rnd(0.25, 0.7)
    });
  }

  function drawFilaments(now) {
    ctx.save();
    ctx.globalAlpha = 0.08;
    ctx.strokeStyle = "rgba(110, 255, 180, 0.75)";
    for (const f of filaments) {
      f.y += f.drift;
      if (f.y > 1.2) f.y = -0.2;
      const x = f.x * c.width;
      const y = f.y * c.height;
      ctx.lineWidth = 1.2;
      ctx.beginPath();
      ctx.moveTo(x - f.w, y - 40);
      ctx.quadraticCurveTo(x, y + 30, x + f.w, y + 120);
      ctx.stroke();
    }
    ctx.restore();
  }

  function drawPulses(now) {
    const cx = c.width * 0.5;
    const cy = c.height * 0.35;
    const maxR = Math.max(c.width, c.height) * 0.6;

    ctx.save();
    for (let i = pulses.length - 1; i >= 0; i--) {
      const p = pulses[i];
      const age = (now - p.t0);
      const r = p.r0 + age * p.speed * 60;
      if (r > maxR) {
        pulses.splice(i, 1);
        continue;
      }
      ctx.globalAlpha = p.alpha * (1 - r / maxR);
      ctx.lineWidth = 1.6;
      ctx.strokeStyle = "rgba(110, 255, 180, 0.65)";
      ctx.beginPath();
      ctx.arc(cx, cy, r, 0, Math.PI * 2);
      ctx.stroke();
    }
    ctx.restore();
  }

  function drawSparks(now) {
    const cx = c.width * 0.5;
    const cy = c.height * 0.5;

    ctx.save();
    ctx.fillStyle = "rgba(210, 255, 230, 0.7)";
    for (const s of sparks) {
      s.r += s.vr;
      if (s.r > Math.max(c.width, c.height) * 0.7) s.r = rnd(30, 120);
      s.ang += 0.0007 + s.vr * 0.0008;
      const x = cx + Math.cos(s.ang) * s.r;
      const y = cy + Math.sin(s.ang) * s.r * 0.6;
      const tw = 0.5 + 0.5 * Math.sin((now / 1000) * s.tw + s.ang);
      ctx.globalAlpha = 0.18 + tw * 0.28;
      ctx.beginPath();
      ctx.arc(x, y, s.size, 0, Math.PI * 2);
      ctx.fill();
    }
    ctx.restore();
  }

  function tick(now) {
    ctx.clearRect(0, 0, c.width, c.height);

    if (now - lastPulse > pulseGapMs) {
      lastPulse = now;
      spawnPulse(now);
    }

    drawFilaments(now);
    drawPulses(now);
    drawSparks(now);

    requestAnimationFrame(tick);
  }

  requestAnimationFrame(tick);
})();

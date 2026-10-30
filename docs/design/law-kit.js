"use strict";
/* Sovereign: общий набор макета «Закон судьбы» (docs/design/law.md).
 *
 * Сборка узлов, значки, подсказка, меню, форма, переезд (FLIP), цвет и
 * контраст. О «Настройках» и «Маршрутах» здесь ничего нет: их знает
 * law-concept.js. Цвета и времена скрипт не придумывает, а читает из
 * tokens.css; своих чисел у него два - задержка подсказки (kTipDelayMs
 * приложения) и ступени лестницы альф, которых в CSS нет как чисел.
 */
(() => {
  const NS = "http://www.w3.org/2000/svg";
  const Law = (window.Law = {});

  /* ---- Ошибки на виду: макет без тихих сбоев -------------------------- */
  window.__errors = [];
  addEventListener("error", (e) => {
    window.__errors.push(`${e.message} (${String(e.filename || "").split("/").pop()}:${e.lineno})`);
  });
  addEventListener("unhandledrejection", (e) => window.__errors.push(`promise: ${e.reason}`));

  /* ---- Токены ---------------------------------------------------------- */
  const root = document.documentElement;
  const cssVar = (name) => getComputedStyle(root).getPropertyValue(name).trim();
  Law.cssVar = cssVar;
  Law.num = (name) => parseFloat(cssVar(name)) || 0; // "160ms" -> 160, "34px" -> 34
  Law.rgb = (name) => cssVar(`--${name}-rgb`).split(/\s+/).map(Number);
  Law.reduced = () => matchMedia("(prefers-reduced-motion: reduce)").matches;
  Law.TIP_DELAY = 500; // kTipDelayMs приложения
  Law.sleep = (ms) => new Promise((done) => setTimeout(done, ms));

  /* ---- Цвет и контраст -------------------------------------------------- */
  const lin = (v) => {
    const c = v / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  const lum = ([r, g, b]) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
  const lab = (rgb) => {
    const [r, g, b] = rgb.map(lin);
    const x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    const y = 0.2126729 * r + 0.7151522 * g + 0.072175 * b;
    const z = (0.0193339 * r + 0.119192 * g + 0.9503041 * b) / 1.08883;
    const f = (t) => (t > 216 / 24389 ? Math.cbrt(t) : ((24389 / 27) * t + 16) / 116);
    return [116 * f(y) - 16, 500 * (f(x) - f(y)), 200 * (f(y) - f(z))];
  };
  Law.color = {
    over: (fg, a, bg) => fg.map((v, i) => v * a + bg[i] * (1 - a)),
    ratio: (x, y) => {
      const a = lum(x);
      const b = lum(y);
      return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
    },
    dE: (x, y) => {
      const p = lab(x);
      const q = lab(y);
      return Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]);
    },
    chroma: (rgb) => Math.max(...rgb) - Math.min(...rgb),
    // "rgb(1, 2, 3)", "rgba(1, 2, 3, .5)", "rgb(1 2 3 / 50%)" -> { rgb, a }
    parse: (s) => {
      const m = /rgba?\(\s*([\d.]+)[ ,]+([\d.]+)[ ,]+([\d.]+)(?:\s*[,/]\s*([\d.]+)(%?))?\s*\)/.exec(s || "");
      if (!m) return null;
      const a = m[4] === undefined ? 1 : m[5] ? Number(m[4]) / 100 : Number(m[4]);
      return { rgb: [Number(m[1]), Number(m[2]), Number(m[3])], a };
    },
  };

  /* ---- Узлы ------------------------------------------------------------- */
  const add = (el, kids) => {
    for (const k of kids.flat(Infinity)) {
      if (k === null || k === undefined || k === false) continue;
      el.append(k.nodeType ? k : document.createTextNode(String(k)));
    }
  };
  Law.h = (tag, props, ...kids) => {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(props || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k.startsWith("on") && typeof v === "function") el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? "" : String(v));
    }
    add(el, kids);
    return el;
  };
  const h = Law.h;
  Law.icon = (name) => {
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("class", "icon");
    svg.setAttribute("aria-hidden", "true");
    svg.setAttribute("focusable", "false");
    const use = document.createElementNS(NS, "use");
    use.setAttribute("href", `#i-${name}`);
    svg.append(use);
    return svg;
  };
  Law.logo = (name) =>
    h("img", { class: "logo", src: `../../assets/services/${name}.svg`, alt: "", width: 16, height: 16, draggable: "false" });
  let counter = 0;
  Law.id = (prefix = "x") => `${prefix}${++counter}`;

  /* Последнее действие: видно глазу строкой, скринридеру его говорит #live. */
  Law.say = (host, text, undo) => {
    const live = document.getElementById("live");
    if (live) {
      live.textContent = "";
      setTimeout(() => (live.textContent = text), 30);
    }
    if (!host) return;
    host.replaceChildren(
      h("span", { class: "dot", "data-verdict": "info", "aria-hidden": "true" }),
      h("span", {}, text),
      undo ? h("button", { class: "muted", type: "button", onclick: undo }, "Вернуть") : null,
    );
  };

  /* ---- Всплывающее: одно за раз ----------------------------------------- */
  // Положение у опоры: ниже, а если не помещается - выше; от краёв окна 8.
  const place = (el, r, { align = "start", gap = 4 } = {}) => {
    const margin = 8;
    el.style.left = "0px";
    el.style.top = "0px";
    const w = el.offsetWidth;
    const hgt = el.offsetHeight;
    const vw = root.clientWidth;
    const vh = root.clientHeight;
    let x = align === "end" ? r.right - w : align === "center" ? r.left + r.width / 2 - w / 2 : r.left;
    x = Math.max(margin, Math.min(x, vw - w - margin));
    let y = r.bottom + gap;
    let side = "below";
    if (y + hgt > vh - margin) {
      if (r.top - gap - hgt >= margin) {
        y = r.top - gap - hgt;
        side = "above";
      } else {
        y = Math.max(margin, vh - hgt - margin);
      }
    }
    el.style.left = `${Math.round(x)}px`;
    el.style.top = `${Math.round(y)}px`;
    return { x: Math.round(x), y: Math.round(y), side };
  };

  let pop = null; // { el, anchor }
  const closePop = (restore) => {
    if (!pop) return;
    const { el, anchor } = pop;
    pop = null;
    el.setAttribute("data-leave", "");
    const d = Law.num("--d-state");
    setTimeout(() => el.remove(), d ? d + 30 : 0);
    if (restore && anchor && anchor.isConnected) anchor.focus({ preventScroll: true });
  };
  const mount = (el, anchor, align) => {
    el.setAttribute("data-enter", "");
    document.body.append(el);
    place(el, anchor.getBoundingClientRect(), { align });
    pop = { el, anchor };
    requestAnimationFrame(() => requestAnimationFrame(() => el.removeAttribute("data-enter")));
  };
  Law.closePop = closePop;
  Law.popOpen = () => pop !== null;

  /* Подсказка: наведение с задержкой 500 мс, фокус клавиатурой - сразу. */
  const tipEl = () => document.getElementById("tip");
  const tip = (Law.tip = { owner: null, pinned: false, timer: 0 });
  const release = () => {
    const o = tip.owner;
    if (!o) return;
    o.removeAttribute("aria-describedby");
    delete o.dataset.open;
    tip.owner = null;
    tip.pinned = false;
  };
  tip.hide = () => {
    clearTimeout(tip.timer);
    tipEl().removeAttribute("data-open");
    release();
  };
  tip.show = (target, text, pinned = false) => {
    clearTimeout(tip.timer);
    if (tip.owner && tip.owner !== target) release();
    const el = tipEl();
    el.replaceChildren(...String(text).split("\n\n").map((p) => h("p", {}, p)));
    el.setAttribute("data-open", "true");
    const r = target.getBoundingClientRect();
    const p = place(el, r, { align: "center", gap: 6 });
    el.dataset.side = p.side;
    el.style.setProperty("--nose", `${Math.round(r.left + r.width / 2 - p.x)}px`);
    target.setAttribute("aria-describedby", "tip");
    target.dataset.open = "true";
    tip.owner = target;
    tip.pinned = pinned;
  };
  tip.attach = (el, text) => {
    const get = () => (typeof text === "function" ? text() : text);
    el.addEventListener("pointerenter", (e) => {
      if (e.pointerType !== "mouse") return;
      clearTimeout(tip.timer);
      tip.timer = setTimeout(() => tip.show(el, get()), Law.TIP_DELAY);
    });
    el.addEventListener("pointerleave", () => {
      clearTimeout(tip.timer);
      if (tip.owner === el && !tip.pinned) tip.hide();
    });
    el.addEventListener("pointerdown", () => {
      clearTimeout(tip.timer);
      if (tip.owner === el && !tip.pinned) tip.hide();
    });
    el.addEventListener("focus", () => {
      if (el.matches(":focus-visible")) tip.show(el, get());
    });
    el.addEventListener("blur", () => {
      clearTimeout(tip.timer);
      if (tip.owner === el) tip.hide();
    });
  };
  // «?»: тот же показ, а нажатие закрепляет подсказку (на сенсорном это единственный способ).
  tip.hint = (el, text) => {
    tip.attach(el, text);
    el.addEventListener("click", () => {
      if (tip.owner === el && tip.pinned) tip.hide();
      else tip.show(el, typeof text === "function" ? text() : text, true);
    });
  };

  /* Меню: пункты 34, стрелки ходят, Esc закрывает и возвращает фокус на ручку.
     items: { text, fate?, logo?, icon?, checked?, disabled? } | { gap: true } | { label }
     (fate - точка судьбы перед словом: пункт «Перенести» читается как легенда доски) */
  Law.menu = ({ anchor, items, label, onPick, align = "start" }) => {
    if (pop && pop.anchor === anchor) {
      closePop(true);
      return;
    }
    closePop(false);
    const el = h("div", { class: "menu", role: "menu", "aria-label": label });
    const focusable = [];
    for (const it of items) {
      if (it.gap) {
        el.append(h("div", { class: "menu-gap", role: "separator" }));
        continue;
      }
      if (it.label) {
        el.append(h("div", { class: "menu-label" }, it.label));
        continue;
      }
      const radio = it.checked !== undefined;
      const b = h(
        "button",
        {
          class: "menu-item",
          type: "button",
          role: radio ? "menuitemradio" : "menuitem",
          "aria-checked": radio ? String(!!it.checked) : null,
          "aria-disabled": it.disabled ? "true" : null,
          tabindex: "-1",
        },
        it.fate ? h("span", { class: "dot", "data-fate": it.fate, "aria-hidden": "true" }) : null,
        it.logo ? Law.logo(it.logo) : null,
        it.icon ? Law.icon(it.icon) : null,
        h("span", { class: "text" }, it.text),
        radio && it.checked ? h("span", { class: "end", "aria-hidden": "true" }, Law.icon("check")) : null,
      );
      if (!it.disabled) {
        b.addEventListener("click", () => {
          closePop(true);
          onPick(it);
        });
        focusable.push(b);
      }
      el.append(b);
    }
    el.addEventListener("keydown", (e) => {
      const i = focusable.indexOf(document.activeElement);
      let j = -1;
      if (e.key === "ArrowDown") j = (i + 1) % focusable.length;
      else if (e.key === "ArrowUp") j = (i - 1 + focusable.length) % focusable.length;
      else if (e.key === "Home") j = 0;
      else if (e.key === "End") j = focusable.length - 1;
      else if (e.key === "Tab") {
        closePop(true);
        return;
      } else return;
      e.preventDefault();
      if (focusable[j]) focusable[j].focus();
    });
    mount(el, anchor, align);
    const first = focusable.find((b) => b.getAttribute("aria-checked") === "true") || focusable[0];
    if (first) first.focus({ preventScroll: true });
    return el;
  };

  /* Форма: подпись, поле, пояснение, ошибка (точка вердикта + слово), две кнопки.
     validate(text) -> { ok: true } | { ok: false, error }. Enter сохраняет, Esc отменяет. */
  Law.form = ({ anchor, label, value = "", placeholder = "", help = "", submit = "Сохранить", validate, onSubmit, align = "start" }) => {
    if (pop && pop.anchor === anchor) {
      closePop(true);
      return;
    }
    closePop(false);
    const fid = Law.id("f");
    const input = h("input", {
      class: "field",
      id: fid,
      type: "text",
      value,
      placeholder,
      autocomplete: "off",
      spellcheck: "false",
      "aria-describedby": `${fid}-help`,
    });
    const errText = h("span", {});
    const err = h("p", { class: "field-error", id: `${fid}-err`, role: "alert", hidden: true }, h("span", { class: "dot", "data-verdict": "danger", "aria-hidden": "true" }), errText);
    const el = h(
      "form",
      { class: "menu", "data-form": "true", role: "dialog", "aria-label": label, novalidate: true },
      h("label", { class: "field-label", for: fid }, label),
      input,
      h("p", { class: "field-help", id: `${fid}-help` }, help),
      err,
      h(
        "div",
        { class: "form-actions" },
        h("button", { class: "btn", type: "button", onclick: () => closePop(true) }, "Отмена"),
        h("button", { class: "btn", type: "submit", "data-kind": "primary" }, submit),
      ),
    );
    el.addEventListener("submit", (e) => {
      e.preventDefault();
      const text = input.value.trim();
      const verdict = validate ? validate(text) : { ok: true };
      if (!verdict.ok) {
        errText.textContent = verdict.error;
        err.hidden = false;
        input.setAttribute("aria-invalid", "true");
        input.setAttribute("aria-describedby", `${fid}-err ${fid}-help`);
        input.focus();
        return;
      }
      closePop(true);
      onSubmit(text);
    });
    input.addEventListener("input", () => {
      err.hidden = true;
      input.removeAttribute("aria-invalid");
    });
    mount(el, anchor, align);
    input.focus({ preventScroll: true });
    input.select();
    return el;
  };

  document.addEventListener(
    "pointerdown",
    (e) => {
      if (pop && !pop.el.contains(e.target) && !pop.anchor.contains(e.target)) closePop(false);
      if (tip.owner && tip.pinned && !tip.owner.contains(e.target)) tip.hide();
    },
    true,
  );
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape") return;
    if (pop) {
      e.preventDefault();
      closePop(true);
    } else if (tip.owner) tip.hide();
  });
  addEventListener("resize", () => {
    closePop(false);
    tip.hide();
  });
  addEventListener("scroll", () => tip.hide(), true);

  /* ---- Радиогруппа: одна точка входа, стрелки ходят и выбирают ----------- */
  Law.radios = (group, onSelect) => {
    const items = () => Array.from(group.querySelectorAll('[role="radio"]'));
    group.addEventListener("keydown", (e) => {
      const list = items();
      const i = list.indexOf(document.activeElement);
      if (i < 0) return;
      let j = i;
      if (e.key === "ArrowRight" || e.key === "ArrowDown") j = (i + 1) % list.length;
      else if (e.key === "ArrowLeft" || e.key === "ArrowUp") j = (i - 1 + list.length) % list.length;
      else if (e.key === "Home") j = 0;
      else if (e.key === "End") j = list.length - 1;
      else return;
      e.preventDefault();
      list[j].focus();
      onSelect(list[j]);
    });
    group.addEventListener("click", (e) => {
      const b = e.target.closest('[role="radio"]');
      if (b) onSelect(b);
    });
  };

  /* ---- Переезд: вещь с data-id приходит из старого места в новое ---------- */
  Law.flip = (scope, sel = "[data-id]") => {
    const before = new Map();
    scope.querySelectorAll(sel).forEach((el) => before.set(el.dataset.id, el.getBoundingClientRect()));
    return () => {
      const d = Law.num("--d-fold");
      if (!d) return; // нет анимации - нет и переезда
      scope.querySelectorAll(sel).forEach((el) => {
        const a = before.get(el.dataset.id);
        if (!a) return;
        const b = el.getBoundingClientRect();
        const dx = a.left - b.left;
        const dy = a.top - b.top;
        if (Math.abs(dx) < 1 && Math.abs(dy) < 1) return;
        el.animate([{ transform: `translate(${dx}px, ${dy}px)` }, { transform: "none" }], {
          duration: d,
          easing: cssVar("--ease"),
        });
      });
    };
  };

  /* Доступное имя: достаточно для самотеста (aria-label, aria-labelledby, текст, alt). */
  Law.accName = (el) => {
    const label = el.getAttribute("aria-label");
    if (label) return label.trim();
    const by = el.getAttribute("aria-labelledby");
    if (by) {
      return by
        .split(/\s+/)
        .map((i) => document.getElementById(i)?.textContent.trim() || "")
        .join(" ")
        .trim();
    }
    const text = el.textContent.trim();
    if (text) return text;
    return (el.querySelector("img[alt]:not([alt=''])")?.getAttribute("alt") || "").trim();
  };
})();

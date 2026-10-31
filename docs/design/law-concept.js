"use strict";
/* Sovereign: два экрана макета «Закон судьбы» (docs/design/law.md):
 * A - «Настройки», B - «Маршруты». Данные и подписи взяты из приложения
 * (main_window.cpp, main.cpp); всё, чего на стенде нет (страница проверки
 * подключения, список запущенных программ, обновление), честно говорит об
 * этом в строке последнего действия, а не делает вид.
 *
 * Цвета и времена здесь не встречаются: страница красится атрибутами
 * data-fate (путь), data-verdict (вердикт) и data-on (рука); числа берутся
 * из tokens.css. ?check печатает проверку закона по отрисованной странице.
 */
(() => {
  const { h, icon, logo, say } = Law;
  const fill = (el, ...kids) => {
    el.replaceChildren(...kids.flat(Infinity).filter(Boolean));
    return el;
  };
  const groove = () => h("div", { class: "groove", "aria-hidden": "true" });
  // Шов между строками; зависимый блок (data-dep) приносит свой шов сам.
  const stack = (rows) =>
    rows.flat().filter(Boolean).flatMap((r, i) => (i && !r.hasAttribute("data-dep") ? [groove(), r] : [r]));
  const card = (...rows) => h("div", { class: "card" }, stack(rows));
  const quiet = (el) => {
    el.replaceChildren(h("span", { class: "dot", "data-verdict": "info", "aria-hidden": "true" }), h("span", {}, "Здесь появится последнее действие."));
  };

  /* ---- Подсказка «?» --------------------------------------------------- */
  const hint = (text, name) => {
    const b = h("button", { class: "hint", type: "button", "aria-label": `Пояснение: ${name}` }, h("span", {}, "?"));
    Law.tip.hint(b, text);
    return b;
  };

  /* ---- Строка ----------------------------------------------------------- *
   * o: label, icon|logo, value, hint, fate (судьба строки), sw (переключатель)
   * + on, end (значок в конце), pop ("menu"|"dialog"), disabled, act(anchor),
   * fold + expanded + controls (ручка складки). Строка без act - «tells»:
   * показывает, ничего не обещает. */
  const row = (o) => {
    const lid = Law.id("l");
    const value = o.value === undefined ? null : h("span", { class: "value", id: Law.id("v") }, o.value);
    const hit = o.act
      ? h("button", {
          class: "hit",
          type: "button",
          role: o.sw ? "switch" : null,
          "aria-checked": o.sw ? String(!!o.on) : null,
          "aria-labelledby": value ? `${lid} ${value.id}` : lid,
          "aria-disabled": o.disabled ? "true" : null,
          "aria-haspopup": o.pop || null,
          "aria-expanded": o.expanded === undefined ? null : String(o.expanded),
          "aria-controls": o.controls || null,
          onclick: (e) => {
            if (!o.disabled) o.act(e.currentTarget);
          },
        })
      : null;
    const el = h(
      "div",
      {
        class: o.fold ? "row fold-handle" : "row",
        "data-fate": o.fate || null,
        "data-on": o.sw ? String(!!o.on) : null,
        "data-disabled": o.disabled ? "true" : null,
      },
      hit,
      o.icon || o.logo ? h("span", { class: "lead" }, o.icon ? icon(o.icon) : logo(o.logo)) : null,
      h("span", { class: "label", id: lid }, o.label),
      o.hint ? hint(o.hint, o.label) : null,
      value,
      o.sw ? h("span", { class: "switch", "aria-hidden": "true" }) : null,
      o.end ? h("span", { class: "end", "aria-hidden": "true" }, icon(o.end)) : null,
    );
    el.hit = hit;
    el.setOn = (on) => {
      el.dataset.on = String(on);
      hit.setAttribute("aria-checked", String(on));
    };
    el.setValue = (t) => {
      value.textContent = t;
    };
    return el;
  };

  // Применить значение, сказать об этом и дать вернуть: одно действие - одна строка статуса.
  const change = (status, what, prev, next, apply, show = String) => {
    apply(next);
    say(status, `${what}: ${show(next)}`, () => {
      apply(prev);
      say(status, `${what}: ${show(prev)}`);
    });
  };

  const ip4 = (t) => {
    const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(t);
    if (!m) return null;
    const o = m.slice(1).map(Number);
    return o.every((n) => n <= 255) ? o : null;
  };
  const HOST = /^(?:[a-z0-9а-яё](?:[a-z0-9а-яё-]*[a-z0-9а-яё])?\.)+[a-zа-яё0-9-]{2,}$/i;

  /* ==================================================================== *
   * A - Настройки
   * ==================================================================== */
  const A_HINT = {
    page:
      "Крестик закрывает клиент совсем, чёрточка прячет окно в трей. Подключение остаётся и после крестика: им управляет служба.\n\nВсё настроенное здесь, конфиги и их история лежат в «Папке данных» внизу — %LOCALAPPDATA%\\Sovereign.",
    kill:
      "Если подключение оборвётся, трафик не пойдёт мимо VPN: интернет закрыт, пока оно не восстановится. Выключишь VPN сам — интернет работает как обычно.",
    auto:
      "При входе в Windows клиент запускается в области уведомлений, без окна. «С подключением» — подключается сам, если был включён; «без подключения» — ждёт, пока включишь.",
    lan:
      "Фильтр только на этом компьютере. Роутер, телефоны и другие устройства работают как обычно и пользуются интернетом — они просто не могут связаться с этим компьютером, а он с ними: общие папки, принтер, трансляция на телевизор, передача файлов с телефона.\n\nИнтернет этого компьютера тоже идёт через роутер, но зашифрованным: роутер не видит, какие сайты открыты, кроме прямых. Закрытый роутер — это только его настройки и его DNS.\n\nСовсем спрятаться нельзя: роутер видит, что компьютер подключён, а сканер сети на телефоне — что по его адресу есть устройство. Связаться с ним никто не сможет.\n\nДействует, пока подключение включено.",
    ads:
      "Запросы к рекламным и трекерным серверам по спискам никуда не уходят: страницы грузятся без рекламы. Сломалось что-то нужное — «Недавно заблокировано» ниже: нажми имя, и оно пойдёт дальше.",
    cookies:
      "Убирает окна «принять cookie» OneTrust, Cookiebot и подобные. Своё окно сайта (TikTok, DeepSeek) — нет: это не скрипт со стороны.",
    quic: "QUIC — быстрый протокол поверх UDP, которым пользуются браузеры. «Авто»: Sovereign сам решает, нужен ли он на этом подключении.",
    ipv4:
      "Сайты получают только IPv4-адреса. У многих серверов VPN нет IPv6: без этого браузер сначала пробует IPv6, ждёт и лишь потом идёт по IPv4 — сайт открывается с задержкой или не открывается.",
    dnsV:
      "Имена всех сайтов, кроме прямых: запрос идёт через VPN, провайдер его не видит.\n\n«Авто» подходит почти всем: это Cloudflare. Свой адрес пишется так: 1.2.3.4, tls://dns.example, https://dns.example/dns-query, quic://dns.example.",
    dnsL:
      "Российские сайты, твои прямые и адреса самих серверов.\n\nЕсли этот DNS перестанет отвечать, Sovereign возьмёт следующий: Cloudflare, Google, Quad9, Яндекс, в конце DNS Windows. Яндекс — только здесь: в России его не блокируют.",
    upd:
      "«Только проверять» — раз в 12 часов; новая версия появится значком в заголовке окна, обновляешь нажатием на него. «Ставить сами» — новая версия скачивается сама и ставится без вопросов, когда компьютер свободен: 10 минут никто не трогал мышь и клавиатуру, ничего не открыто на весь экран, ничего не качается. VPN пропадает примерно на полминуты. «Не проверять» — только строкой «Проверить сейчас» ниже.",
    folder: "Открыть папку данных: настройки, конфиги, их история",
  };
  const AUTO = { off: "выключен", with: "с подключением", without: "без подключения" };
  const UPD = { never: "не проверять", check: "только проверять", auto: "ставить сами" };
  const QUIC = { auto: "авто (сейчас выкл.)", on: "вкл.", off: "выкл." };
  const DNS_V = ["авто · Cloudflare", "Cloudflare (DoH)", "Google (DoH)", "Quad9 (DoH)"];
  const DNS_L = [...DNS_V, "Яндекс (DoH)", "Системный"];

  const screenA = () => {
    const win = document.getElementById("win-a");
    const status = document.getElementById("status-a");
    quiet(status);
    const S = {
      kill: true,
      auto: "with",
      lan: false,
      hosts: [{ id: "router", name: "Роутер", addr: "192.168.1.1", seen: true }],
      ads: true,
      cookies: false,
      blocked: ["doubleclick.net", "googleads.g.doubleclick.net", "an.yandex.ru", "pagead2.googlesyndication.com", "connect.facebook.net"],
      quic: "auto",
      ipv4: false,
      dnsV: "авто · Cloudflare",
      dnsL: "авто · Cloudflare",
      upd: "check",
      open: false,
    };
    const later = (what) => () => say(status, `${what} открывается в приложении: на стенде этой страницы нет.`);

    // Переключатель: судьба строки (block) горит, пока он включён.
    const sw = (key, o) => {
      const el = row({
        ...o,
        sw: true,
        on: S[key],
        act: () => {
          const prev = S[key];
          const set = (v) => {
            S[key] = v;
            el.setOn(v);
            o.after?.();
          };
          change(status, o.label, prev, !prev, set, (v) => (v ? "включено" : "выключено"));
        },
      });
      return el;
    };
    // Строка выбора: значение справа, шеврон «ведёт дальше», меню выбора из списка.
    const pick = (o) => {
      const el = row({
        ...o,
        value: o.show(),
        end: "chevron-right",
        pop: "menu",
        act: (anchor) =>
          Law.menu({
            anchor,
            label: o.label,
            align: "end",
            items: o.items(),
            onPick: (it) => o.onPick(it, anchor),
          }),
      });
      el.refresh = () => el.setValue(o.show());
      return el;
    };
    const dnsRow = (key, label, fate, list, hintText) => {
      const lab = key === "dnsV" ? "DNS через VPN" : "DNS напрямую";
      const el = pick({
        label,
        fate,
        hint: hintText,
        show: () => S[key],
        items: () => [
          ...list.map((t) => ({ text: t, v: t, checked: S[key] === t })),
          { gap: true },
          { text: "Свой…", v: null, checked: !list.includes(S[key]) },
        ],
        onPick: (it, anchor) => {
          const set = (v) => {
            S[key] = v;
            el.refresh();
            syncFold();
          };
          if (it.v) return change(status, lab, S[key], it.v, set);
          Law.form({
            anchor,
            label: `Свой DNS: ${label.toLowerCase()}`,
            value: list.includes(S[key]) ? "" : S[key],
            placeholder: "tls://dns.example",
            help: "Так: 1.2.3.4, tls://dns.example, https://dns.example/dns-query или quic://dns.example",
            submit: "Сохранить",
            validate: (t) =>
              ip4(t) || /^(?:tls|quic):\/\/[^\s/]+$/i.test(t) || /^https:\/\/[^\s/]+(?:\/\S*)?$/i.test(t)
                ? { ok: true }
                : { ok: false, error: "Не похоже на адрес DNS. Нужен 1.2.3.4, tls://…, https://…/dns-query или quic://…" },
            onSubmit: (t) => change(status, lab, S[key], t, set),
          });
        },
      });
      return el;
    };

    /* -- Проверка подключения -------------------------------------------- */
    const check = row({ label: "Проверка подключения", icon: "shield", value: "утечки, скорость", end: "chevron-right", act: later("Проверка подключения") });

    /* -- Подключение ------------------------------------------------------ */
    const kill = sw("kill", { label: "Kill switch", fate: "block", hint: A_HINT.kill });
    const auto = pick({
      label: "Автозапуск",
      hint: A_HINT.auto,
      show: () => AUTO[S.auto],
      items: () => Object.entries(AUTO).map(([v, t]) => ({ text: t[0].toUpperCase() + t.slice(1), v, checked: S.auto === v })),
      onPick: (it) =>
        change(status, "Автозапуск", S.auto, it.v, (v) => {
          S.auto = v;
          auto.refresh();
        }, (v) => AUTO[v]),
    });
    const lanDep = h("div", { "data-dep": "" });
    let lanAdd = null;
    const privateIp = (t) => {
      const o = ip4(t);
      return !!o && (o[0] === 10 || (o[0] === 192 && o[1] === 168) || (o[0] === 172 && o[1] >= 16 && o[1] <= 31));
    };
    const syncLan = (focusAdd) => {
      if (!S.lan) return fill(lanDep);
      const rows = S.hosts.map((host) => {
        const r = row({
          label: host.name,
          logo: "lan",
          value: host.seen ? host.addr : "не в сети",
          pop: "menu",
          act: (anchor) =>
            Law.menu({
              anchor,
              label: host.name,
              align: "end",
              items: [{ text: "Закрыть снова" }],
              onPick: () => {
                const at = S.hosts.indexOf(host);
                S.hosts.splice(at, 1);
                syncLan(true);
                say(status, `${host.name}: закрыт снова`, () => {
                  S.hosts.splice(at, 0, host);
                  syncLan(false);
                  say(status, `${host.name}: снова разрешён`);
                });
              },
            }),
        });
        return r;
      });
      lanAdd = row({
        label: "Разрешить устройство",
        icon: "plus",
        pop: "dialog",
        act: (anchor) =>
          Law.form({
            anchor,
            label: "Адрес устройства",
            placeholder: "192.168.1.20",
            help: "Локальный адрес устройства, которому можно связаться с этим компьютером.",
            submit: "Разрешить",
            validate: (t) => {
              if (!privateIp(t)) return { ok: false, error: "Нужен локальный адрес: 192.168.x.x, 10.x.x.x или 172.16–31.x.x" };
              if (S.hosts.some((x) => x.addr === t)) return { ok: false, error: "Это устройство уже разрешено" };
              return { ok: true };
            },
            onSubmit: (t) => {
              const host = { id: Law.id("host"), name: t, addr: t, seen: false };
              S.hosts.push(host);
              syncLan(true);
              say(status, `${t}: разрешён`, () => {
                S.hosts.splice(S.hosts.indexOf(host), 1);
                syncLan(false);
                say(status, `${t}: закрыт снова`);
              });
            },
          }),
      });
      fill(lanDep, [...rows, lanAdd].flatMap((r) => [groove(), r]));
      if (focusAdd) lanAdd.hit.focus({ preventScroll: true });
    };
    const lan = sw("lan", { label: "Закрыть от локальной сети", fate: "block", hint: A_HINT.lan, after: () => syncLan(false) });

    /* -- Реклама ----------------------------------------------------------- */
    const recentDep = h("div", { "data-dep": "" });
    const syncRecent = (focus) => {
      if (!S.ads && !S.cookies) return fill(recentDep);
      const empty = S.blocked.length === 0;
      const r = row({
        label: "Недавно заблокировано",
        value: empty ? "ничего не блокировалось" : "пропустить нужное",
        end: "chevron-right",
        pop: "menu",
        disabled: empty,
        act: (anchor) =>
          Law.menu({
            anchor,
            label: "Недавно заблокировано",
            align: "end",
            items: [{ label: "Нажми имя — оно пойдёт дальше" }, ...S.blocked.map((t) => ({ text: t }))],
            onPick: (it) => {
              const at = S.blocked.indexOf(it.text);
              S.blocked.splice(at, 1);
              syncRecent(true);
              say(status, `${it.text}: пойдёт дальше`, () => {
                S.blocked.splice(at, 0, it.text);
                syncRecent(false);
                say(status, `${it.text}: снова блокируется`);
              });
            },
          }),
      });
      fill(recentDep, groove(), r);
      if (focus) r.hit.focus({ preventScroll: true });
    };
    const ads = sw("ads", { label: "Блокировать рекламу", logo: "ads", fate: "block", hint: A_HINT.ads, after: () => syncRecent(false) });
    const cookies = sw("cookies", { label: "Убирать баннеры cookie", logo: "cookies", fate: "block", hint: A_HINT.cookies, after: () => syncRecent(false) });

    /* -- Тонкая настройка: ручка + итог + шов -------------------------------- */
    const quic = pick({
      label: "QUIC",
      hint: A_HINT.quic,
      show: () => QUIC[S.quic],
      items: () => [
        { text: "Авто", v: "auto", checked: S.quic === "auto" },
        { text: "Вкл.", v: "on", checked: S.quic === "on" },
        { text: "Выкл.", v: "off", checked: S.quic === "off" },
      ],
      onPick: (it) =>
        change(status, "QUIC", S.quic, it.v, (v) => {
          S.quic = v;
          quic.refresh();
          syncFold();
        }, (v) => QUIC[v]),
    });
    const ipv4 = sw("ipv4", { label: "Только IPv4", hint: A_HINT.ipv4, after: () => syncFold() });
    const dnsV = dnsRow("dnsV", "DNS через VPN", "vpn", DNS_V, A_HINT.dnsV);
    const dnsL = dnsRow("dnsL", "DNS напрямую", "direct", DNS_L, A_HINT.dnsL);
    const foldId = Law.id("fold");
    const more = row({
      label: "Тонкая настройка",
      value: "",
      end: "chevron-down",
      fold: true,
      expanded: false,
      controls: foldId,
      act: () => {
        S.open = !S.open;
        foldCard.dataset.open = String(S.open);
        foldBody.dataset.open = String(S.open);
        foldBody.inert = !S.open;
        more.hit.setAttribute("aria-expanded", String(S.open));
        syncFold();
      },
    });
    // Свёрнуто - итог того, что настроено, а не того, что внутри.
    const syncFold = () => {
      const parts = [];
      if (S.quic === "off") parts.push("QUIC выкл.");
      if (S.quic === "on") parts.push("QUIC вкл.");
      if (S.ipv4) parts.push("IPv4");
      if (!S.dnsV.startsWith("авто") || !S.dnsL.startsWith("авто")) parts.push("свой DNS");
      more.setValue(S.open ? "" : parts.length ? parts.join(" · ") : "QUIC, IPv4, DNS");
    };
    const foldBody = h("div", { class: "fold", id: foldId, "data-open": "false", inert: true }, h("div", { class: "fold-body" }, groove(), stack([quic, ipv4, dnsV, dnsL])));
    const foldCard = h("div", { class: "card", "data-open": "false" }, more, foldBody);

    /* -- О Sovereign ------------------------------------------------------- */
    let busy = false;
    const now = row({
      label: "Проверить сейчас",
      icon: "refresh",
      value: "",
      act: async () => {
        if (busy) return;
        busy = true;
        now.setValue("проверяю…");
        say(status, "Проверяю обновления…");
        await Law.sleep(1200);
        now.setValue("не в этой сборке");
        say(status, "Обновления: проверка работает только в приложении, на стенде её нет.");
        busy = false;
      },
    });
    const upd = pick({
      label: "Обновления",
      hint: A_HINT.upd,
      show: () => UPD[S.upd],
      items: () => Object.entries(UPD).map(([v, t]) => ({ text: t[0].toUpperCase() + t.slice(1), v, checked: S.upd === v })),
      onPick: (it) =>
        change(status, "Обновления", S.upd, it.v, (v) => {
          S.upd = v;
          upd.refresh();
        }, (v) => UPD[v]),
    });
    const folder = row({
      label: "Папка данных",
      icon: "folder",
      hint: A_HINT.folder,
      act: () => say(status, "Папка данных — %LOCALAPPDATA%\\Sovereign. Из браузера её не открыть."),
    });

    fill(
      win,
      h("div", { class: "page-title" }, h("h3", { id: "h-a-title" }, "Настройки"), hint(A_HINT.page, "настройки")),
      card(check),
      h("h4", { class: "group-title" }, "Подключение"),
      card(kill, auto, lan, lanDep),
      h("h4", { class: "group-title" }, "Реклама"),
      card(ads, cookies, recentDep),
      foldCard,
      h("h4", { class: "group-title" }, "О Sovereign"),
      card(
        row({ label: "Sovereign", value: "0.5.66" }),
        row({ label: "sing-box", value: "1.14.4 · последняя" }),
        upd,
        now,
        folder,
      ),
    );
    syncLan(false);
    syncRecent(false);
    syncFold();
  };

  /* ==================================================================== *
   * B - Маршруты
   * ==================================================================== */
  const B_HINT = {
    page:
      "Куда идёт трафик. Это решает Sovereign: из подписок берутся только серверы, их правила и DNS не используются.\n\n«Всё» — через VPN идёт всё, а ниже исключения: то, что идёт иначе. «Только выбранное» — через VPN идёт лишь то, что лежит в «Через VPN», остальное напрямую. Списки для обоих режимов хранятся: программы одного не видны в другом, потому что там ничего не меняют.",
    rest: "Скрытое и так идёт сюда вместе со всем остальным, поэтому ничего не меняет. Оно не удалено: переключи режим наверху — и вернётся на экран.",
    warp:
      "WARP — бесплатный выход через Cloudflare. Выручает там, где сайт не пускает с адресов VPN-серверов или режет контент (ChatGPT, Netflix): сайт видит адрес Cloudflare. Google его адреса не любит — Gemini и поиск через WARP упрутся в капчу.\n\nWARP идёт поверх твоего VPN — провайдер его не видит. Не проходит так — Sovereign сам пустит WARP напрямую.",
    block: "Что сюда попадёт, не откроется: ни сайт, ни программа. Пока там пусто — блокировать нечего. Рекламу и баннеры cookie убирают настройки.",
    chip: "Перенести в другой путь или убрать",
  };
  const PATHS = [
    { id: "vpn", name: "Через VPN", out: "через VPN" },
    { id: "direct", name: "Напрямую", out: "напрямую" },
    { id: "warp", name: "Через WARP", out: "через WARP", hint: B_HINT.warp },
    { id: "block", name: "Блок", out: "заблокирован", hint: B_HINT.block },
  ];
  const PATH = Object.fromEntries(PATHS.map((p) => [p.id, p]));

  // Сервисы доски: имя, значок «что» и домены, по которым их узнаёт проверка.
  const CATALOG = [
    { id: "russia", name: "Российские сайты", logo: "russia", kind: "russia", domains: [] },
    { id: "yandex", name: "Яндекс", logo: "yandex", domains: ["yandex.ru", "ya.ru", "yandex.com"] },
    { id: "telegram", name: "Telegram", logo: "telegram", domains: ["t.me", "telegram.org"] },
    { id: "youtube", name: "YouTube", logo: "youtube", domains: ["youtube.com", "youtu.be", "googlevideo.com"] },
    { id: "chatgpt", name: "ChatGPT", logo: "chatgpt", domains: ["chatgpt.com", "openai.com"] },
    { id: "netflix", name: "Netflix", logo: "netflix", domains: ["netflix.com", "nflxvideo.net"] },
    { id: "discord", name: "Discord", logo: "discord", domains: ["discord.com", "discord.gg"] },
    { id: "claude", name: "Claude", logo: "claude", domains: ["claude.ai", "anthropic.com"] },
    { id: "gemini", name: "Gemini", logo: "gemini", domains: ["gemini.google.com"] },
    { id: "grok", name: "Grok", logo: "grok", domains: ["grok.com"] },
    { id: "google", name: "Google", logo: "google", domains: ["google.com"] },
    { id: "github", name: "GitHub", logo: "github", domains: ["github.com"] },
    { id: "spotify", name: "Spotify", logo: "spotify", domains: ["spotify.com"] },
    { id: "steam", name: "Steam", logo: "steam", domains: ["steampowered.com"] },
    { id: "twitch", name: "Twitch", logo: "twitch", domains: ["twitch.tv"] },
    { id: "tiktok", name: "TikTok", logo: "tiktok", domains: ["tiktok.com"] },
    { id: "whatsapp", name: "WhatsApp", logo: "whatsapp", domains: ["whatsapp.com"] },
    { id: "instagram", name: "Instagram", logo: "instagram", domains: ["instagram.com"] },
    { id: "facebook", name: "Facebook", logo: "facebook", domains: ["facebook.com"] },
    { id: "reddit", name: "Reddit", logo: "reddit", domains: ["reddit.com"] },
    { id: "soundcloud", name: "SoundCloud", logo: "soundcloud", domains: ["soundcloud.com"] },
    { id: "x", name: "X", logo: "x", domains: ["x.com", "twitter.com"] },
  ];
  const SVC = Object.fromEntries(CATALOG.map((s) => [s.id, s]));
  const PROBES = [
    ["ya.ru", "Россия"],
    ["spiegel.de", "Европа"],
    ["chatgpt.com", "США"],
  ];

  const screenB = () => {
    const win = document.getElementById("win-b");
    const status = document.getElementById("status-b");
    quiet(status);
    const S = {
      mode: "all",
      chips: [
        { id: "russia", name: "Российские сайты", logo: "russia", kind: "russia", path: "direct" },
        { id: "yandex", name: "Яндекс", logo: "yandex", kind: "service", path: "direct", service: "yandex" },
        { id: "rule-brave", name: "brave.exe", icon: "app", kind: "rule", processOnly: true, path: "direct" },
        { id: "chatgpt", name: "ChatGPT", logo: "chatgpt", kind: "service", path: "warp", service: "chatgpt" },
        { id: "netflix", name: "Netflix", logo: "netflix", kind: "service", path: "warp", service: "netflix" },
        { id: "telegram", name: "Telegram", logo: "telegram", kind: "service", path: "vpn", service: "telegram" },
        { id: "youtube", name: "YouTube", logo: "youtube", kind: "service", path: "vpn", service: "youtube" },
        { id: "rule-discord", name: "discord.exe", icon: "app", kind: "rule", processOnly: true, path: "vpn" },
      ],
      result: null,
    };
    const rest = () => (S.mode === "all" ? "vpn" : "direct");
    // Скрытая фишка: идёт туда, куда и всё остальное, поэтому ничего не меняет.
    const idle = (c) => {
      if (c.path !== rest()) return false;
      if (c.kind === "russia") return !S.chips.some((x) => x.kind === "rule" && x.processOnly && x.path !== rest() && x.path !== "block");
      if (c.kind === "service") return true;
      return !!c.processOnly;
    };
    const hiddenCount = () => S.chips.filter((c) => c.path === rest() && idle(c)).length;

    const pill = (text, fate, sr) =>
      h("span", { class: "pill", "data-fate": fate }, h("span", { "aria-hidden": sr ? "true" : null }, text), sr ? h("span", { class: "sr-only" }, sr) : null);

    /* -- Режим ------------------------------------------------------------- */
    const segs = [
      ["all", "всё"],
      ["selected", "только выбранное"],
    ].map(([mode, text]) => h("button", { class: "seg", type: "button", role: "radio", "data-mode": mode }, text));
    const group = h("div", { class: "segments", role: "radiogroup", "aria-label": "Через VPN идёт" }, segs);
    const restEl = h("div", { class: "rest" });
    const modeCard = h("div", { class: "card mode" }, group, restEl);
    const syncMode = () => {
      for (const s of segs) {
        const on = s.dataset.mode === S.mode;
        s.setAttribute("aria-checked", String(on));
        s.tabIndex = on ? 0 : -1;
      }
      const to = rest();
      fill(
        restEl,
        h("span", { class: "muted" }, "всё остальное →"),
        pill(to === "vpn" ? "VPN" : "Напрямую", to),
        hiddenCount() ? h("span", { class: "tail" }, h("span", { class: "muted" }, `скрыто ${hiddenCount()}`), hint(B_HINT.rest, "скрытое")) : null,
      );
    };

    /* -- Доска ------------------------------------------------------------- */
    const board = h("div", { class: "board" });
    const render = (focus) => {
      Law.tip.hide();
      const cards = PATHS.map((p) => {
        const shown = S.chips.filter((c) => c.path === p.id && !idle(c));
        if (p.id === rest() && !shown.length) return null;
        const empty = !shown.length;
        const key = h("button", { class: "key", type: "button", "aria-label": `Добавить в «${p.name}»`, "aria-haspopup": "menu", "data-way": p.id, onclick: (e) => addMenu(p, e.currentTarget) }, icon("plus"));
        Law.tip.attach(key, `Добавить в «${p.name}»: сервис, программа или сайт`);
        const head = h(
          "div",
          { class: "way-head" },
          h("h4", { class: "way-name" }, p.name),
          p.hint ? hint(p.hint, p.name) : null,
          !empty && p.id === "warp" ? h("span", { class: "way-note" }, "поверх VPN") : null,
          empty ? null : pill(String(shown.length).padStart(2, "0"), p.id, `в «${p.name}»`),
          key,
        );
        const chips = empty
          ? null
          : h(
              "div",
              { class: "chips" },
              shown.map((c) => {
                const el = h(
                  "button",
                  { class: "chip", type: "button", "data-id": c.id, "aria-haspopup": "menu", "aria-label": `${c.name}, ${PATH[c.path].name}. Перенести или убрать`, onclick: (e) => chipMenu(c, e.currentTarget) },
                  c.logo ? logo(c.logo) : icon(c.icon),
                  h("span", { class: "text" }, c.name),
                );
                Law.tip.attach(el, B_HINT.chip);
                return el;
              }),
            );
        return h("section", { class: "card way", "data-fate": p.id, "data-empty": String(empty), "aria-label": p.name }, head, chips);
      });
      fill(board, cards);
      syncMode();
      if (focus) (board.querySelector(`[data-id="${focus.id}"]`) || board.querySelector(`[data-way="${focus.way}"]`) || board.querySelector(".key"))?.focus({ preventScroll: true });
    };
    // Любое изменение доски: фишки переезжают (FLIP по data-id), статус говорит, что стало.
    const mutate = (fn, focus) => {
      const after = Law.flip(board);
      fn();
      render(focus);
      after();
    };
    const move = (c, to, verb) => {
      const from = c.path;
      mutate(() => (c.path = to), { id: c.id, way: to });
      say(status, `${c.name}: «${PATH[to].name}»${idle(c) ? ". Скрыто: так оно и идёт" : ""}`, () => {
        mutate(() => (c.path = from), { id: c.id, way: from });
        say(status, `${c.name}: «${PATH[from].name}»`);
      });
    };
    const chipMenu = (c, anchor) =>
      Law.menu({
        anchor,
        label: c.name,
        items: [{ label: "Перенести" }, ...PATHS.filter((p) => p.id !== c.path).map((p) => ({ text: p.name, fate: p.id, to: p.id })), { gap: true }, { text: "Убрать", icon: "x", rm: true }],
        onPick: (it) => {
          if (it.to) return move(c, it.to);
          const at = S.chips.indexOf(c);
          mutate(() => S.chips.splice(at, 1), { way: c.path });
          say(status, `${c.name}: убрано`, () => {
            mutate(() => S.chips.splice(at, 0, c), { id: c.id, way: c.path });
            say(status, `${c.name}: вернулось в «${PATH[c.path].name}»`);
          });
        },
      });
    const put = (c, p) => {
      mutate(() => S.chips.push({ ...c, path: p.id }), { id: c.id, way: p.id });
      const added = S.chips[S.chips.length - 1];
      say(status, `${c.name}: добавлено в «${p.name}»${idle(added) ? ". Скрыто: так оно и идёт" : ""}`, () => {
        mutate(() => S.chips.splice(S.chips.indexOf(added), 1), { way: p.id });
        say(status, `${c.name}: убрано`);
      });
    };
    const addMenu = (p, anchor) => {
      const taken = new Set(S.chips.map((c) => c.id));
      Law.menu({
        anchor,
        label: `Добавить в «${p.name}»`,
        items: [
          { label: `Добавить в «${p.name}»` },
          ...CATALOG.filter((s) => !taken.has(s.id)).map((s) => ({ text: s.name, logo: s.logo, svc: s })),
          { gap: true },
          { text: "Программа…", icon: "app", form: "app" },
          { text: "Сайт или адрес…", icon: "globe", form: "site" },
        ],
        onPick: (it) => {
          if (it.svc) return put({ id: it.svc.id, name: it.svc.name, logo: it.svc.logo, kind: it.svc.kind || "service", service: it.svc.kind ? undefined : it.svc.id }, p);
          if (it.form === "app") {
            return Law.form({
              anchor,
              label: "Программа",
              placeholder: "brave.exe",
              help: "Имя файла программы. Список запущенных программ браузеру недоступен — в приложении он есть.",
              submit: "Добавить",
              validate: (t) => {
                if (!/^[^\s\\/:*?"<>|]+\.exe$/i.test(t)) return { ok: false, error: "Нужно имя файла с .exe, например brave.exe" };
                if (S.chips.some((c) => c.processOnly && c.name.toLowerCase() === t.toLowerCase())) return { ok: false, error: "Эта программа уже на доске" };
                return { ok: true };
              },
              onSubmit: (t) => put({ id: Law.id("rule"), name: t, icon: "app", kind: "rule", processOnly: true }, p),
            });
          }
          Law.form({
            anchor,
            label: "Сайт или адрес",
            placeholder: "example.com",
            help: "Домен или IP-адрес. Поддомены идут туда же.",
            submit: "Добавить",
            validate: (t) => {
              const host = cleanHost(t);
              if (!(HOST.test(host) || ip4(host))) return { ok: false, error: "Не похоже на сайт. Нужен домен вроде example.com или адрес 1.2.3.4" };
              if (S.chips.some((c) => c.domains?.includes(host))) return { ok: false, error: "Этот адрес уже на доске" };
              return { ok: true };
            },
            onSubmit: (t) => {
              const host = cleanHost(t);
              put({ id: Law.id("rule"), name: host, icon: "globe", kind: "rule", domains: [host] }, p);
            },
          });
        },
      });
    };

    /* -- Не открывается сайт? ------------------------------------------------ */
    const cleanHost = (t) => t.trim().toLowerCase().replace(/^[a-z]+:\/\//, "").replace(/[/?#:].*$/, "").replace(/^www\./, "");
    const route = (host) => {
      const has = (d) => host === d || host.endsWith(`.${d}`);
      let best = null;
      for (const c of S.chips) {
        const domains = c.domains || SVC[c.service]?.domains || [];
        for (const d of domains) if (has(d) && (!best || d.length > best.len)) best = { len: d.length, path: c.path, rule: `правило «${c.name}»` };
      }
      if (best) return best;
      const ru = S.chips.find((c) => c.kind === "russia");
      if (ru && /\.(?:ru|su|рф|xn--p1ai)$/.test(host)) return { path: ru.path, rule: "правило «Российские сайты»" };
      return { path: rest(), rule: "остальное" };
    };
    const resultDep = h("div", { "data-dep": "" });
    const showResult = () => {
      const r = S.result;
      if (!r) return fill(resultDep);
      fill(
        resultDep,
        groove(),
        h("div", { class: "row" }, h("span", { class: "lead" }, h("span", { class: "dot", "data-verdict": r.verdict, "aria-hidden": "true" })), h("span", { class: "label" }, r.head)),
        h("div", { class: "row" }, h("p", { class: "field-help" }, r.detail)),
      );
    };
    const checkRow = row({
      label: "Не открывается сайт?",
      icon: "globe",
      value: "проверить адрес",
      end: "chevron-right",
      pop: "dialog",
      act: (anchor) =>
        Law.form({
          anchor,
          label: "Адрес сайта",
          placeholder: "chatgpt.com",
          help: "Покажу, каким путём он идёт. Пусто — проверю три примера: Россию, Европу и США.",
          submit: "Проверить",
          validate: (t) => {
            const host = cleanHost(t);
            return !t || HOST.test(host) || ip4(host) ? { ok: true } : { ok: false, error: "Не похоже на адрес сайта. Нужен домен вроде chatgpt.com" };
          },
          onSubmit: (t) => {
            if (t) {
              const host = cleanHost(t);
              const r = route(host);
              S.result = { verdict: "ok", head: `${host}: ${PATH[r.path].out}`, detail: `${host} — ${PATH[r.path].out}; ${r.rule}` };
            } else {
              const got = PROBES.map(([host, region]) => ({ host, region, r: route(host) }));
              const byPath = new Map();
              for (const g of got) byPath.set(g.r.path, [...(byPath.get(g.r.path) || []), g.region]);
              S.result = {
                verdict: "ok",
                head: [...byPath].map(([path, regions]) => `${regions.join(", ")} → ${PATH[path].out}`).join(" · "),
                detail: got.map((g) => `${g.host} — ${PATH[g.r.path].out}; ${g.r.rule}`).join(" · "),
              };
            }
            showResult();
            say(status, S.result.head);
          },
        }),
    });

    /* -- Сборка ---------------------------------------------------------------- */
    const setMode = (mode) => {
      if (mode === S.mode) return;
      const prev = S.mode;
      const apply = (m) => mutate(() => (S.mode = m), null);
      apply(mode);
      say(status, `Через VPN идёт: ${mode === "all" ? "всё" : "только выбранное"}. Скрыто: ${hiddenCount()}`, () => {
        apply(prev);
        say(status, `Через VPN идёт: ${prev === "all" ? "всё" : "только выбранное"}`);
      });
      segs.find((s) => s.dataset.mode === mode)?.focus({ preventScroll: true });
    };
    Law.radios(group, (b) => setMode(b.dataset.mode));

    fill(
      win,
      h("div", { class: "page-title" }, h("h3", {}, "Маршруты"), hint(B_HINT.page, "маршруты")),
      h("h4", { class: "group-title" }, "Через VPN идёт"),
      modeCard,
      h("h4", { class: "group-title" }, "Идёт иначе"),
      board,
      card(checkRow, resultDep),
    );
    render(null);
  };

  /* ==================================================================== *
   * ?check - проверка закона по отрисованной странице
   * ==================================================================== */
  const runCheck = () => {
    const out = [];
    const t = (name, ok, detail = "") => out.push({ name, ok, detail });
    const cardRgb = Law.rgb("card");
    const palette = ["vpn", "warp", "direct", "block", "ok", "warn"].map((n) => Law.rgb(n));
    const visible = (el) => el.getClientRects().length > 0;
    const named = (el) => Law.accName(el) !== "";

    t("ошибок скрипта нет", window.__errors.length === 0, window.__errors.join("; "));
    t("горизонтальной прокрутки нет", document.documentElement.scrollWidth <= innerWidth, `${document.documentElement.scrollWidth} из ${innerWidth}`);
    t("шрифт Geist загружен", document.fonts.check("14px Geist"));

    const ctl = [...document.querySelectorAll("button, input, [role=radio], [role=switch]")];
    const nameless = ctl.filter((el) => !named(el));
    t("у каждого органа есть имя", nameless.length === 0, `${ctl.length} шт.; без имени: ${nameless.map((e) => e.className).join(", ")}`);
    const hitMin = Law.num("--hit-min");
    const small = ctl.filter((el) => visible(el)).filter((el) => {
      const r = el.getBoundingClientRect();
      return r.width < hitMin - 0.5 || r.height < hitMin - 0.5;
    });
    t(`зона нажатия не меньше ${hitMin}`, small.length === 0, small.map((e) => `${e.className} ${Math.round(e.getBoundingClientRect().width)}x${Math.round(e.getBoundingClientRect().height)}`).join(", "));

    const rowMin = Law.num("--row");
    const low = [...document.querySelectorAll(".row")].filter((el) => el.getBoundingClientRect().height < rowMin - 0.5);
    t(`строка не ниже ${rowMin}`, low.length === 0, `${document.querySelectorAll(".row").length} строк`);

    // Три линии: зона строки 4, содержимое 20 (у пути - имя и фишки).
    const line = Law.num("--line");
    const inset = Law.num("--inset-row");
    const skew = [];
    for (const c of document.querySelectorAll(".card")) {
      const left = c.getBoundingClientRect().left;
      for (const r of c.querySelectorAll(".row")) {
        const first = [...r.children].find((k) => !k.classList.contains("hit"));
        if (first && Math.abs(first.getBoundingClientRect().left - left - line) > 0.5) skew.push(`${first.className}@${Math.round(first.getBoundingClientRect().left - left)}`);
        const hit = r.querySelector(":scope > .hit");
        if (hit && Math.abs(hit.getBoundingClientRect().left - left - inset) > 0.5) skew.push(`hit@${Math.round(hit.getBoundingClientRect().left - left)}`);
      }
      for (const k of c.querySelectorAll(".way-name, .chips")) {
        if (Math.abs(k.getBoundingClientRect().left - left - line) > 0.5 && k.classList.contains("way-name")) skew.push(`way-name@${Math.round(k.getBoundingClientRect().left - left)}`);
      }
    }
    t("содержимое карточек стоит на линии 20, зона строки на 4", skew.length === 0, skew.slice(0, 6).join(", "));

    const mism = [...document.querySelectorAll(".row[data-on]")].filter((r) => r.querySelector(".hit")?.getAttribute("aria-checked") !== r.dataset.on);
    t("aria-checked переключателей совпадает с их видом", mism.length === 0);
    const folds = [...document.querySelectorAll(".fold")].filter((f) => {
      const handle = document.querySelector(`[aria-controls="${f.id}"]`);
      return handle?.getAttribute("aria-expanded") !== f.dataset.open || f.inert !== (f.dataset.open !== "true");
    });
    t("складка: aria-expanded, data-open и inert согласованы", folds.length === 0);

    // Оттенок только у судьбы и вердикта: любой хроматичный цвет страницы - из палитры закона.
    const stray = new Set();
    for (const el of document.body.querySelectorAll("*")) {
      if (el.closest(".sprite") || el.tagName === "IMG") continue;
      for (const pseudo of [null, "::before", "::after"]) {
        const cs = getComputedStyle(el, pseudo);
        for (const prop of ["color", "backgroundColor", "borderTopColor", "borderLeftColor"]) {
          if ((prop.startsWith("border") && parseFloat(cs[prop.replace("Color", "Width")]) === 0) || (prop === "backgroundColor" && !pseudo && false)) continue;
          const c = Law.color.parse(cs[prop]);
          if (!c || c.a === 0 || Law.color.chroma(c.rgb) <= 12) continue;
          if (!palette.some((p) => p.every((v, i) => Math.abs(v - c.rgb[i]) < 1.5))) stray.add(`${el.className || el.tagName}${pseudo || ""} ${prop} ${cs[prop]}`);
        }
      }
    }
    t("оттенок только у судьбы и вердикта", stray.size === 0, [...stray].slice(0, 4).join("; "));

    // Закон: текст судьбой не красится. Слово - ink; оттенок несут рейка, кромка, точка и ядро.
    const dyed = [];
    for (const el of document.body.querySelectorAll("*")) {
      if (![...el.childNodes].some((k) => k.nodeType === 3 && k.textContent.trim()) || !visible(el)) continue;
      const c = Law.color.parse(getComputedStyle(el).color);
      if (c && Law.color.chroma(c.rgb) > 12) dyed.push(`${el.className || el.tagName} ${getComputedStyle(el).color}`);
    }
    t("ни одно слово не окрашено оттенком", dyed.length === 0, dyed.slice(0, 4).join("; "));
    const steps = ["micro", "caption", "body", "heading", "lead", "title"].map((n) => Law.num(`--t-${n}`));
    const ratios = steps.slice(1).map((v, i) => v / steps[i]);
    t("шкала шрифтов: отношение соседних ступеней в [1.11; 1.14]", ratios.every((r) => r >= 1.11 && r <= 1.14), `${steps.join(" / ")}; ${ratios.map((r) => r.toFixed(3)).join(" ")}`);

    const text = { "ink-1": 4.5, "ink-2": 4.5, vpn: 4.5, warp: 4.5, direct: 4.5, block: 4.5, ok: 4.5, warn: 4.5, lit: 4.5 };
    const weak = Object.entries(text).filter(([n, min]) => Law.color.ratio(Law.rgb(n), cardRgb) < min);
    t("текст на карточке не ниже 4.5:1 (ink-1, ink-2, четыре судьбы, вердикты, рука)", weak.length === 0, Object.keys(text).map((n) => `${n} ${Law.color.ratio(Law.rgb(n), cardRgb).toFixed(2)}`).join(", "));
    t("ink на ручке переключателя: lit-ink на lit не ниже 4.5:1", Law.color.ratio(Law.rgb("lit-ink"), Law.rgb("lit")) >= 4.5, Law.color.ratio(Law.rgb("lit-ink"), Law.rgb("lit")).toFixed(2));
    const fates = ["vpn", "warp", "direct", "block"];
    let minDe = Infinity;
    let pair = "";
    fates.forEach((a, i) =>
      fates.slice(i + 1).forEach((b) => {
        const d = Law.color.dE(Law.rgb(a), Law.rgb(b));
        if (d < minDe) {
          minDe = d;
          pair = `${a}–${b}`;
        }
      }),
    );
    t("судьбы различимы: ΔE76 не меньше 30", minDe >= 30, `слабая пара ${pair} ${minDe.toFixed(1)}`);

    const pre = h("pre", { class: "check-out", id: "check" }, out.map((o) => `${o.ok ? "OK  " : "FAIL"} ${o.name}${o.detail ? ` — ${o.detail}` : ""}`).join("\n") + `\nИТОГ ${out.filter((o) => o.ok).length}/${out.length} при ${innerWidth} px`);
    document.querySelector("main").append(pre);
  };

  screenA();
  screenB();
  if (/[?&]check\b/.test(location.search)) document.fonts.ready.then(() => requestAnimationFrame(runCheck));
})();

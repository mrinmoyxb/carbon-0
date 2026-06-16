(() => {
  const STATS_ENDPOINT = "http://localhost:8888/browser-usage";
  const ENERGY_PER_1K_TOKENS_WH = 3.0;
  const CARBON_INTENSITY_G_PER_KWH = 400.0;
  const SCAN_INTERVAL_MS = 2500;

  let lastSentPromptTokens = 0;
  let lastSentCompletionTokens = 0;
  let lastConversationKey = "";
  let lastStatus = "starting";

  function getPlatform() {
    const host = location.hostname;
    if (host.includes("chatgpt.com") || host.includes("chat.openai.com")) return "chatgpt";
    if (host.includes("gemini.google.com")) return "gemini";
    if (host.includes("claude.ai")) return "claude";
    return "unknown";
  }

  function conversationKey() {
    return `carbon-pet:${location.hostname}:${location.pathname}`;
  }

  function estimateTokens(text) {
    const normalized = (text || "").replace(/\s+/g, " ").trim();
    if (!normalized) return 0;
    const cjkChars = (normalized.match(/[\u3400-\u9fff]/g) || []).length;
    const otherChars = normalized.length - cjkChars;
    return Math.max(1, Math.ceil(cjkChars + otherChars / 4));
  }

  function co2ForTokens(tokens) {
    const energyWh = (tokens / 1000) * ENERGY_PER_1K_TOKENS_WH;
    return energyWh * CARBON_INTENSITY_G_PER_KWH / 1000;
  }

  function getMessages() {
    const platform = getPlatform();
    const messages = [];

    if (platform === "chatgpt") {
      document.querySelectorAll("[data-message-author-role]").forEach(node => {
        messages.push({
          role: node.getAttribute("data-message-author-role"),
          text: node.innerText || node.textContent || ""
        });
      });
    }

    else if (platform === "gemini") {
      document.querySelectorAll(".query-text-inner").forEach(node => {
        const text = (node.innerText || "").replace(/^You said\s*/i, "").trim();
        if (text) messages.push({ role: "user", text });
      });
      document.querySelectorAll(".model-response-text").forEach(node => {
        const text = (node.innerText || "").replace(/^Gemini said\s*/i, "").trim();
        if (text) messages.push({ role: "assistant", text });
      });
    }

    else if (platform === "claude") {
      // User messages — stable test ID
      document.querySelectorAll("[data-testid='user-message']").forEach(node => {
        const text = (node.innerText || "").trim();
        if (text) messages.push({ role: "user", text });
      });
      // Assistant messages — Claude streams into .font-claude-message containers
      document.querySelectorAll(".font-claude-message").forEach(node => {
        const text = (node.innerText || "").trim();
        if (text) messages.push({ role: "assistant", text });
      });
    }

    return messages.filter(m => m.text.trim().length > 0);
  }

  function currentTotals() {
    const totals = { promptTokens: 0, completionTokens: 0 };
    for (const msg of getMessages()) {
      const tokens = estimateTokens(msg.text);
      if (msg.role === "user") totals.promptTokens += tokens;
      else if (msg.role === "assistant") totals.completionTokens += tokens;
    }
    return totals;
  }

  // ── Badge styles ─────────────────────────────────────────────────────────

  const PLATFORM_STYLE = {
    chatgpt: { label: "ChatGPT", bg: "rgba(16,163,127,0.18)", color: "#34d399", border: "rgba(16,163,127,0.3)" },
    gemini:  { label: "Gemini",  bg: "rgba(99,102,241,0.18)", color: "#a5b4fc", border: "rgba(99,102,241,0.3)" },
    claude:  { label: "Claude",  bg: "rgba(213,130,70,0.18)", color: "#fdba74", border: "rgba(213,130,70,0.3)" },
    unknown: { label: "?",       bg: "rgba(120,120,120,0.18)", color: "#aaa",   border: "rgba(120,120,120,0.3)" }
  };

  function ensureBadge() {
    let badge = document.getElementById("carbon-pet-badge");
    if (badge) return badge;

    const style = document.createElement("style");
    style.id = "carbon-pet-style";
    style.textContent = `
      #carbon-pet-badge {
        position: fixed; right: 16px; bottom: 16px; z-index: 2147483647;
        width: 196px;
        background: #0d1f15;
        border: 0.5px solid rgba(80,220,130,0.22);
        border-radius: 14px;
        padding: 10px 13px;
        font: 400 11px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
        color: #a3f0c0;
        box-shadow: inset 0 0 0 0.5px rgba(80,220,130,0.07);
        pointer-events: none;
        transition: opacity 0.3s;
      }
      #carbon-pet-badge .cp-header {
        display: flex; align-items: center; gap: 6px; margin-bottom: 8px;
      }
      #carbon-pet-badge .cp-leaf {
        font-size: 14px; color: #4ade80; line-height: 1;
      }
      #carbon-pet-badge .cp-title {
        font-size: 10px; font-weight: 700; letter-spacing: 0.05em;
        text-transform: uppercase; color: #a3f0c0;
      }
      #carbon-pet-badge .cp-chip {
        margin-left: auto;
        font-size: 9.5px; font-weight: 700; letter-spacing: 0.03em;
        padding: 2px 7px; border-radius: 99px;
      }
      #carbon-pet-badge .cp-divider {
        border: none; border-top: 0.5px solid rgba(80,220,130,0.12); margin: 0 0 8px;
      }
      #carbon-pet-badge .cp-row {
        display: flex; justify-content: space-between; align-items: baseline;
        margin-bottom: 4px;
      }
      #carbon-pet-badge .cp-label {
        font-size: 10px; color: #5a8a6a; letter-spacing: 0.02em;
      }
      #carbon-pet-badge .cp-val {
        font-size: 13px; font-weight: 600; color: #e2ffe9;
        font-variant-numeric: tabular-nums;
      }
      #carbon-pet-badge .cp-co2 {
        font-size: 11px; font-weight: 500; color: #86efac;
      }
      #carbon-pet-badge .cp-footer {
        display: flex; align-items: center; gap: 5px;
        margin-top: 8px; padding-top: 7px;
        border-top: 0.5px solid rgba(80,220,130,0.10);
      }
      #carbon-pet-badge .cp-dot {
        width: 6px; height: 6px; border-radius: 50%; flex-shrink: 0;
      }
      #carbon-pet-badge .cp-dot.on  { background: #4ade80; box-shadow: 0 0 0 2px rgba(74,222,128,0.22); }
      #carbon-pet-badge .cp-dot.off { background: #4b5563; }
      #carbon-pet-badge .cp-status {
        font-size: 10px; color: #5a8a6a;
      }
    `;
    (document.head || document.documentElement).appendChild(style);

    badge = document.createElement("div");
    badge.id = "carbon-pet-badge";
    badge.innerHTML = `
      <div class="cp-header">
        <span class="cp-leaf">🌿</span>
        <span class="cp-title">Carbon Pet</span>
        <span class="cp-chip" id="cp-chip"></span>
      </div>
      <hr class="cp-divider">
      <div class="cp-row">
        <span class="cp-label">Est. tokens</span>
        <span class="cp-val" id="cp-tokens">—</span>
      </div>
      <div class="cp-row">
        <span class="cp-label">CO₂</span>
        <span class="cp-co2" id="cp-co2">—</span>
      </div>
      <div class="cp-footer">
        <div class="cp-dot off" id="cp-dot"></div>
        <span class="cp-status" id="cp-status">starting…</span>
      </div>
    `;
    document.documentElement.appendChild(badge);
    return badge;
  }

  function updateBadge(totals) {
    const badge = ensureBadge();
    const platform = getPlatform();
    const ps = PLATFORM_STYLE[platform] || PLATFORM_STYLE.unknown;
    const totalTokens = totals.promptTokens + totals.completionTokens;
    const co2 = co2ForTokens(totalTokens);

    const chip = badge.querySelector("#cp-chip");
    chip.textContent = ps.label;
    chip.style.background = ps.bg;
    chip.style.color = ps.color;
    chip.style.border = `0.5px solid ${ps.border}`;

    badge.querySelector("#cp-tokens").textContent = totalTokens.toLocaleString();
    badge.querySelector("#cp-co2").textContent = co2.toFixed(5) + "g";

    const dot = badge.querySelector("#cp-dot");
    const statusEl = badge.querySelector("#cp-status");
    const isConnected = lastStatus === "connected";
    dot.className = "cp-dot " + (isConnected ? "on" : "off");
    statusEl.textContent = lastStatus;
  }

  // ── Network ───────────────────────────────────────────────────────────────

  async function sendDelta(deltaPromptTokens, deltaCompletionTokens) {
    const totalTokens = deltaPromptTokens + deltaCompletionTokens;
    if (totalTokens <= 0) return;

    const platform = getPlatform();
    const payload = {
      source: `${platform}-extension`,
      model: `${location.hostname}-visible-estimate`,
      page_url: location.href,
      prompt_tokens: deltaPromptTokens,
      completion_tokens: deltaCompletionTokens,
      total_tokens: totalTokens
    };

    const response = await fetch(STATS_ENDPOINT, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    });

    if (!response.ok) throw new Error(`Carbon Pet endpoint returned ${response.status}`);
  }

  async function scanAndReport() {
    if (getPlatform() === "unknown") return;

    const key = conversationKey();
    if (key !== lastConversationKey) {
      lastConversationKey = key;
      lastSentPromptTokens = 0;
      lastSentCompletionTokens = 0;
    }

    const totals = currentTotals();

    if (
      totals.promptTokens < lastSentPromptTokens ||
      totals.completionTokens < lastSentCompletionTokens
    ) {
      lastSentPromptTokens = 0;
      lastSentCompletionTokens = 0;
    }

    const deltaPrompt     = Math.max(0, totals.promptTokens     - lastSentPromptTokens);
    const deltaCompletion = Math.max(0, totals.completionTokens - lastSentCompletionTokens);

    try {
      await sendDelta(deltaPrompt, deltaCompletion);
      lastSentPromptTokens     += deltaPrompt;
      lastSentCompletionTokens += deltaCompletion;
      lastStatus = "connected";
    } catch {
      lastStatus = "desktop offline";
    }

    updateBadge(totals);
  }

  // ── DOM observer ──────────────────────────────────────────────────────────

  const observer = new MutationObserver(() => {
    window.clearTimeout(observer._timer);
    observer._timer = window.setTimeout(scanAndReport, 500);
  });

  observer.observe(document.documentElement, {
    childList: true,
    subtree: true,
    characterData: true
  });

  window.setInterval(scanAndReport, SCAN_INTERVAL_MS);
  scanAndReport();
})();
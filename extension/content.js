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
      // User messages — stable test ID (set intentionally by Claude's team)
      document.querySelectorAll("[data-testid='user-message']").forEach(node => {
        const text = (node.innerText || "").trim();
        if (text) messages.push({ role: "user", text });
      });

      // Assistant messages — try selectors from most to least stable:
      // 1. Stable test ID  (preferred)
      // 2. .prose containers inside the response column (structural, not Tailwind-generated)
      // 3. Original .font-claude-message fallback (breaks when they rebuild CSS)
      const byTestId = document.querySelectorAll("[data-testid='assistant-message']");
      const byProse  = document.querySelectorAll(".prose");
      const byClass  = document.querySelectorAll(".font-claude-message");

      const assistantNodes = byTestId.length > 0 ? byTestId
                           : byProse.length  > 0 ? byProse
                           : byClass;

      if (assistantNodes.length > 0) {
        assistantNodes.forEach(node => {
          // Skip any node that lives inside a user-message to avoid double-counting quoted text
          if (node.closest("[data-testid='user-message']")) return;
          const text = (node.innerText || "").trim();
          if (text) messages.push({ role: "assistant", text });
        });
      } else {
        // Can't detect response nodes via specific selectors.
        // Fall back to scraping all visible text in the main content area
        // and treating it as assistant tokens, tagged as "LLM".
        const mainContent = document.querySelector("main") || document.body;
        // Exclude nav, header, footer, and the input box to avoid noise
        const cloneNode = mainContent.cloneNode(true);
        ["nav", "header", "footer", "[contenteditable]", "textarea", "button"].forEach(sel => {
          cloneNode.querySelectorAll(sel).forEach(n => n.remove());
        });
        const fallbackText = (cloneNode.innerText || cloneNode.textContent || "").trim();
        if (fallbackText) {
          messages.push({ role: "assistant", text: fallbackText });
        } else {
          // Absolutely nothing readable — push a minimal marker so the LLM tag still appears
          messages.push({ role: "assistant", text: "LLM" });
        }
      }
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
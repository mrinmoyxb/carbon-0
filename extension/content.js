(() => {
  const STATS_ENDPOINT = "http://localhost:8888/browser-usage";
  const ENERGY_PER_1K_TOKENS_WH = 3.0;
  const CARBON_INTENSITY_G_PER_KWH = 400.0;
  const SCAN_INTERVAL_MS = 2500;

  let lastSentPromptTokens = 0;
  let lastSentCompletionTokens = 0;
  let lastConversationKey = "";
  let lastStatus = "Starting";

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
    const nodes = Array.from(document.querySelectorAll("[data-message-author-role]"));
    return nodes
      .map((node) => ({
        role: node.getAttribute("data-message-author-role"),
        text: node.innerText || node.textContent || ""
      }))
      .filter((message) => message.text.trim().length > 0);
  }

  function currentTotals() {
    const totals = {
      promptTokens: 0,
      completionTokens: 0
    };

    for (const message of getMessages()) {
      const tokens = estimateTokens(message.text);
      if (message.role === "user") {
        totals.promptTokens += tokens;
      } else if (message.role === "assistant") {
        totals.completionTokens += tokens;
      }
    }

    return totals;
  }

  function ensureBadge() {
    let badge = document.getElementById("carbon-pet-extension-badge");
    if (badge) return badge;

    badge = document.createElement("div");
    badge.id = "carbon-pet-extension-badge";
    badge.style.cssText = [
      "position: fixed",
      "right: 16px",
      "bottom: 16px",
      "z-index: 2147483647",
      "padding: 8px 10px",
      "border-radius: 8px",
      "background: rgba(20, 38, 29, 0.92)",
      "color: #f2fff7",
      "font: 600 12px/1.35 -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif",
      "box-shadow: 0 8px 24px rgba(0, 0, 0, 0.22)",
      "pointer-events: none",
      "white-space: pre-line"
    ].join(";");
    document.documentElement.appendChild(badge);
    return badge;
  }

  function updateBadge(totals) {
    const totalTokens = totals.promptTokens + totals.completionTokens;
    const co2 = co2ForTokens(totalTokens);
    const badge = ensureBadge();
    badge.textContent = `Carbon Pet: ${totalTokens} est. tokens\n${co2.toFixed(5)}g CO2 | ${lastStatus}`;
  }

  async function sendDelta(deltaPromptTokens, deltaCompletionTokens) {
    const totalTokens = deltaPromptTokens + deltaCompletionTokens;
    if (totalTokens <= 0) return;

    const payload = {
      source: "chatgpt-extension",
      model: "chatgpt.com-visible-estimate",
      page_url: location.href,
      prompt_tokens: deltaPromptTokens,
      completion_tokens: deltaCompletionTokens,
      total_tokens: totalTokens
    };

    const response = await fetch(STATS_ENDPOINT, {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify(payload)
    });

    if (!response.ok) {
      throw new Error(`Carbon Pet endpoint returned ${response.status}`);
    }
  }

  async function scanAndReport() {
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

    const deltaPromptTokens = Math.max(0, totals.promptTokens - lastSentPromptTokens);
    const deltaCompletionTokens = Math.max(0, totals.completionTokens - lastSentCompletionTokens);

    try {
      await sendDelta(deltaPromptTokens, deltaCompletionTokens);
      lastSentPromptTokens += deltaPromptTokens;
      lastSentCompletionTokens += deltaCompletionTokens;
      lastStatus = "connected";
    } catch (error) {
      lastStatus = "desktop offline";
    }

    updateBadge(totals);
  }

  const observer = new MutationObserver(() => {
    window.clearTimeout(observer._carbonPetTimer);
    observer._carbonPetTimer = window.setTimeout(scanAndReport, 500);
  });

  observer.observe(document.documentElement, {
    childList: true,
    subtree: true,
    characterData: true
  });

  window.setInterval(scanAndReport, SCAN_INTERVAL_MS);
  scanAndReport();
})();

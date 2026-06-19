# Carbon Pet Chrome Extension

This extension estimates visible ChatGPT usage and sends token deltas to the local Carbon Pet proxy at:

```text
http://localhost:8888/browser-usage
```

## Install locally

1. Run the desktop app and local receiver:

   ```bash
   make run-dev
   ```

2. Open Chrome and go to:

   ```text
   chrome://extensions
   ```

3. Enable `Developer mode`.
4. Click `Load unpacked`.
5. Select this folder:

   ```text
   path/to/carbon-0/extension
   ```

6. Open or refresh `https://chatgpt.com`.

The page will show a small Carbon Pet badge in the bottom-right corner. The desktop pet will update through the local proxy stats endpoint.

## Accuracy

This is an estimate based on visible prompt and assistant text. It cannot see hidden system prompts, backend retries, tool calls, file parsing, image processing, or exact model tokenization.

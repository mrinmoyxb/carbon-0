#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import "webview.h"
#import <iostream>
#import <string>

// This helper function searches the macOS window's view hierarchy
// to find the WKWebView (WebKit Browser View) and makes its background transparent.
void makeWebViewTransparent(NSView *view) {
    if ([view isKindOfClass:NSClassFromString(@"WKWebView")]) {
        WKWebView *webView = (WKWebView *)view;
        // KVC (Key-Value Coding) to set drawsBackground to false
        [webView setValue:@(NO) forKey:@"drawsBackground"];
        
        // Set underPageBackgroundColor to clear to prevent white flashes when scrolling
        if ([webView respondsToSelector:@selector(setUnderPageBackgroundColor:)]) {
            webView.underPageBackgroundColor = [NSColor clearColor];
        }
        return;
    }
    // Recursively search in all subviews
    for (NSView *subview in [view subviews]) {
        makeWebViewTransparent(subview);
    }
}

int main() {
    // 1. Initialize the webview instance.
    // First parameter (true) enables Web Inspector (Developer Tools) so you can inspect CSS/HTML.
    // Second parameter (nullptr) tells it to create a new window automatically.
    webview::webview w(true, nullptr);
    
    // 2. Set the window title and size.
    // We use WEBVIEW_HINT_FIXED so the user can't resize it manually.
    w.set_title("Carbon Pet");
    w.set_size(300, 300, WEBVIEW_HINT_FIXED);

    // 3. Cast the generic window handle to a Cocoa NSWindow pointer
    NSWindow *window = (NSWindow *)w.window().value();
    if (window != nil) {
        // NSWindowStyleMaskBorderless removes the title bar and close buttons
        [window setStyleMask:NSWindowStyleMaskBorderless];
        
        // Make the window itself transparent
        [window setBackgroundColor:[NSColor clearColor]];
        [window setOpaque:NO];
        
        // Keep a drop shadow around our glassmorphic card
        [window setHasShadow:YES];
        
        // Make the window float on top of other applications
        [window setLevel:NSFloatingWindowLevel];
        
        // Allow dragging the window from any part of its background
        [window setMovableByWindowBackground:YES];
        
        // Find and transparentize the webview widget inside the window's view hierarchy
        makeWebViewTransparent([window contentView]);
    }

    // 4. Get the absolute path to our index.html file and load it
    char absolute_path[PATH_MAX];
    if (realpath("ui/index.html", absolute_path) != nullptr) {
        std::string url = "file://" + std::string(absolute_path);
        w.navigate(url);
    } else {
        w.set_html("<html><body style='background:transparent;'><h3>Error: ui/index.html not found</h3></body></html>");
    }

    // 5. Run the application main loop (blocks until the window is closed)
    w.run();

    return 0;
}

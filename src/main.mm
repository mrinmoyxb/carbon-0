#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import "webview.h"
#import <iostream>
#import <string>
#import <thread>
#import <chrono>
#import <atomic>
#import <mach/mach_init.h>
#import <mach/mach_error.h>
#import <mach/mach_host.h>
#import <mach/vm_map.h>
#import <ApplicationServices/ApplicationServices.h>

std::atomic<bool> g_running(true);

// --- Power Tracker Code ---
const double IDLE_WATTAGE = 1.0;
const double MAX_CPU_WATTAGE = 15.0;
const double CARBON_INTENSITY_G_PER_KWH = 400.0;

struct CpuTicks {
    unsigned long long user;
    unsigned long long system;
    unsigned long long idle;
    unsigned long long nice;
};

bool getCpuTicks(CpuTicks& ticks) {
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    host_cpu_load_info_data_t cpu_load;
    
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_CPU_LOAD_INFO,
                                         (host_info64_t)&cpu_load, &count);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    
    ticks.user = cpu_load.cpu_ticks[CPU_STATE_USER];
    ticks.system = cpu_load.cpu_ticks[CPU_STATE_SYSTEM];
    ticks.idle = cpu_load.cpu_ticks[CPU_STATE_IDLE];
    ticks.nice = cpu_load.cpu_ticks[CPU_STATE_NICE];
    return true;
}

void powerTrackerLoop(webview::webview* w) {
    CpuTicks prev_ticks = {0, 0, 0, 0};
    getCpuTicks(prev_ticks);
    double total_emissions_g = 0.0;
    
    while (g_running) {
        // Sleep for 1 second in chunks so we can exit faster if needed
        for (int i = 0; i < 10 && g_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!g_running) break;
        
        CpuTicks curr_ticks;
        if (!getCpuTicks(curr_ticks)) continue;
        
        unsigned long long total_user = curr_ticks.user - prev_ticks.user;
        unsigned long long total_system = curr_ticks.system - prev_ticks.system;
        unsigned long long total_idle = curr_ticks.idle - prev_ticks.idle;
        unsigned long long total_nice = curr_ticks.nice - prev_ticks.nice;
        
        unsigned long long total_delta = total_user + total_system + total_idle + total_nice;
        double cpu_usage_pct = 0.0;
        if (total_delta > 0) {
            cpu_usage_pct = (double)(total_user + total_system + total_nice) / total_delta;
        }
        
        double current_wattage = IDLE_WATTAGE + (cpu_usage_pct * MAX_CPU_WATTAGE);
        double kwh_this_second = current_wattage / 3600000.0; 
        double co2_this_second = kwh_this_second * CARBON_INTENSITY_G_PER_KWH;
        total_emissions_g += co2_this_second;
        
        // Calculate system idle time
        CFTimeInterval idleTime = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGAnyInputEventType);
        double idleSeconds = (double)idleTime;
        
        // Dispatch to UI thread to update JavaScript
        w->dispatch([w, current_wattage, total_emissions_g, idleSeconds]() {
            std::string js = "window.updateMetrics(" + 
                             std::to_string(current_wattage) + ", " + 
                             std::to_string(total_emissions_g) + ", " +
                             std::to_string(idleSeconds) + ");";
            w->eval(js);
        });
        
        prev_ticks = curr_ticks;
    }
}
// ----------------------------

// This helper function searches the macOS window's view hierarchy
// to find the WKWebView (WebKit Browser View) and makes its background transparent.
void makeWebViewTransparent(NSView *view) {
    if ([view isKindOfClass:NSClassFromString(@"WKWebView")]) {
        WKWebView *webView = (WKWebView *)view;
        [webView setValue:@(NO) forKey:@"drawsBackground"];
        if ([webView respondsToSelector:@selector(setUnderPageBackgroundColor:)]) {
            webView.underPageBackgroundColor = [NSColor clearColor];
        }
        return;
    }
    for (NSView *subview in [view subviews]) {
        makeWebViewTransparent(subview);
    }
}

int main() {
    webview::webview w(true, nullptr);
    w.set_title("Carbon Pet");
    w.set_size(260, 400, WEBVIEW_HINT_FIXED);

    NSWindow *window = (NSWindow *)w.window().value();
    if (window != nil) {
        [window setStyleMask:NSWindowStyleMaskBorderless];
        [window setBackgroundColor:[NSColor clearColor]];
        [window setOpaque:NO];
        // Remove shadow since we removed the card background, let koala's shadow pop
        [window setHasShadow:NO]; 
        [window setLevel:NSFloatingWindowLevel];
        [window setMovableByWindowBackground:YES];
        makeWebViewTransparent([window contentView]);
    }

    // Bind JS function to handle dragging
    w.bind("startWindowDrag", [&w](std::string s) -> std::string {
        dispatch_async(dispatch_get_main_queue(), ^{
            NSWindow *window = (NSWindow *)w.window().value();
            NSEvent *event = [NSApp currentEvent];
            if (event && window) {
                [window performWindowDragWithEvent:event];
            }
        });
        return "";
    });

    char absolute_path[PATH_MAX];
    if (realpath("ui/index.html", absolute_path) != nullptr) {
        std::string url = "file://" + std::string(absolute_path);
        w.navigate(url);
    } else {
        w.set_html("<html><body style='background:transparent;color:white;'><h3>Error: ui/index.html not found</h3></body></html>");
    }

    // Start tracker thread
    std::thread trackerThread(powerTrackerLoop, &w);

    w.run();

    // Clean up
    g_running = false;
    if (trackerThread.joinable()) {
        trackerThread.join();
    }

    return 0;
}

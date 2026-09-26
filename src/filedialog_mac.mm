// filedialog.h on macOS: NSOpenPanel / NSSavePanel, run modally. GLFW has
// already made this process a Cocoa application, which is all the panels need.
#include "filedialog.h"

#import <AppKit/AppKit.h>

#include <string>
#include <vector>

namespace filedialog {
namespace {

// "*.png;*.jpg" -> {"png", "jpg"}. NSOpenPanel filters by extension, not by
// glob; an empty result means any file.
NSArray<NSString*>* extensionsOf(const std::vector<Filter>& filters) {
    NSMutableArray<NSString*>* out = [NSMutableArray array];
    for (const Filter& f : filters) {
        NSString* all = [NSString stringWithUTF8String:f.patterns ? f.patterns : ""];
        for (NSString* part in [all componentsSeparatedByString:@";"]) {
            NSString* p = [part stringByTrimmingCharactersInSet:
                                    [NSCharacterSet whitespaceCharacterSet]];
            if ([p hasPrefix:@"*."]) p = [p substringFromIndex:2];
            if (p.length && ![p isEqualToString:@"*"]) [out addObject:p];
        }
    }
    return out;
}

void applyFilters(NSSavePanel* panel, const std::vector<Filter>& filters) {
    NSArray<NSString*>* exts = extensionsOf(filters);
    if (!exts.count) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // allowedFileTypes takes plain extensions on every macOS this supports;
    // its replacement needs UTTypes, which exist only from macOS 11 SDKs on.
    panel.allowedFileTypes = exts;
#pragma clang diagnostic pop
}

std::string pathOf(NSURL* url) {
    return url.fileSystemRepresentation ? std::string(url.fileSystemRepresentation)
                                        : std::string();
}

// The GLFW window loses key status to a modal panel and does not always get
// it back by itself.
void restoreFocus(NSWindow* previous) {
    if (previous) [previous makeKeyAndOrderFront:nil];
}

std::vector<std::string> runOpen(const char* title, const std::vector<Filter>& filters,
                                 bool multi, bool folders) {
    std::vector<std::string> out;
    @autoreleasepool {
        NSWindow* previous = NSApp.keyWindow;
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        if (title) panel.message = [NSString stringWithUTF8String:title];
        panel.canChooseFiles = !folders;
        panel.canChooseDirectories = folders;
        panel.allowsMultipleSelection = multi;
        if (!folders) applyFilters(panel, filters);
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL* url in panel.URLs) {
                std::string p = pathOf(url);
                if (!p.empty()) out.push_back(p);
            }
        }
        restoreFocus(previous);
    }
    return out;
}

} // namespace

void init() {}

std::string openFile(const char* title, const std::vector<Filter>& filters) {
    std::vector<std::string> r = runOpen(title, filters, /*multi=*/false, /*folders=*/false);
    return r.empty() ? std::string() : r.front();
}

std::vector<std::string> openFiles(const char* title, const std::vector<Filter>& filters) {
    return runOpen(title, filters, /*multi=*/true, /*folders=*/false);
}

std::string pickFolder(const char* title) {
    std::vector<std::string> r = runOpen(title, {}, /*multi=*/false, /*folders=*/true);
    return r.empty() ? std::string() : r.front();
}

std::string saveFile(const char* title, const std::vector<Filter>& filters,
                     const char* defaultName, const char* defaultExt) {
    std::string out;
    @autoreleasepool {
        NSWindow* previous = NSApp.keyWindow;
        NSSavePanel* panel = [NSSavePanel savePanel];
        if (title) panel.message = [NSString stringWithUTF8String:title];
        if (defaultName && *defaultName)
            panel.nameFieldStringValue = [NSString stringWithUTF8String:defaultName];
        applyFilters(panel, filters);
        // With a filter set, the panel appends the extension itself when the
        // user leaves it off -- the job defaultExt does on Windows.
        (void)defaultExt;
        if ([panel runModal] == NSModalResponseOK) out = pathOf(panel.URL);
        restoreFocus(previous);
    }
    return out;
}

} // namespace filedialog

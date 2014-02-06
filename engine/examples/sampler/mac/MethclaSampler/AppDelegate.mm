//
//  AppDelegate.m
//  MethclaSamplerOSX
//
//  Created by Stefan Kersten on 27/08/2013.
//  Copyright (c) 2013 Samplecount. All rights reserved.
//

#import "AppDelegate.h"
#import "Engine.hpp"

#include <methcla/plugins/soundfile_api_libsndfile.h>
#include <vector>

using namespace Methcla::Examples::Sampler;

static NSArray * openFiles()
{
    NSOpenPanel * panel = [NSOpenPanel openPanel];
    [panel setAllowsMultipleSelection:YES];
    [panel setCanChooseDirectories:NO];
    [panel setCanChooseFiles:YES];
    [panel setAllowedFileTypes: [NSArray arrayWithObjects:@"wav",@"aiff",nil]];
    [panel setFloatingPanel:YES];
    NSInteger result = [panel runModal];
    return result == NSOKButton ? [panel URLs] : nil;
}

static std::vector<unichar> keys = {
    122,97,113,120,115,119,99,100,101,118,102,114,98,103,116,110,104,121,109,106,117,44,107,105,46,108,111,47,59,112
};

static long keyToIndex(unichar key)
{
//    switch (key) {
//    case 122: return 0;
//    case 120: return 1;
//    case  99: return 2;
//    case 118: return 3;
//    case  98: return 4;
//    case  97: return 5;
//    case 115: return 6;
//    case 100: return 7;
//    case 102: return 8;
//    case 103: return 9;
//    case 113: return 10;
//    case 119: return 11;
//    case 101: return 12;
//    case 114: return 13;
//    case 116: return 14;
//    }
    auto it = std::find(keys.begin(), keys.end(), key);
    return it == keys.end() ? -1 : it - keys.begin();
}

@interface KeyboardView : NSView
{
    Engine* engine;
}
- (void)setEngine:(Engine*)theEngine;
@end

@implementation KeyboardView
- (void)setEngine:(Engine*)theEngine
{
    engine = theEngine;
}
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (void)keyDown:(NSEvent *)theEvent
{
    if (![theEvent isARepeat]) {
        NSString* chars = [theEvent charactersIgnoringModifiers];
        if ([chars length] == 1) {
            unichar key = [chars characterAtIndex:0];
            long index = keyToIndex(key);
//            NSLog(@"keyDown: key=%u index%d", key, index);
//            std::cout << key << ",";
            if (index >= 0) {
                engine->startVoice(static_cast<intptr_t>(index), index, 0.2);
            }
        }
    }
}
- (void)keyUp:(NSEvent *)theEvent
{
    if (![theEvent isARepeat]) {
        NSString* chars = [theEvent charactersIgnoringModifiers];
        if ([chars length] == 1) {
            unichar key = [chars characterAtIndex:0];
//            NSLog(@"keyUp: %u", key);
            int index = keyToIndex(key);
            if (index >= 0) {
                engine->stopVoice(static_cast<intptr_t>(index));
            }
        }
    }
}
@end

inline NSString* resourcePath(NSString* component)
{
    return [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:component];
}

@interface AppDelegate ()
{
    Engine* engine;
}
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
    Engine::Options options;
    options.engineOptions.audioDriver.bufferSize = 128;
    options.engineOptions.addLibrary(methcla_soundfile_api_libsndfile);

    NSArray* sounds = openFiles();
    options.sounds.reserve([sounds count]);
    for (id url in sounds)
    {
        options.sounds.push_back([[url path] UTF8String]);
    }

    // Set up the sound engine
    try {
        // Initialize and configure the audio engine
//        options.soundDir = [resourcePath(@"sounds") UTF8String];
        engine = new Engine(options);
        engine->useDisk(true);
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
    }

    // Set up a KeyboardView as first responder
    KeyboardView* view = [[KeyboardView alloc] initWithFrame:NSMakeRect(100, 100, 100, 100)];
    [view setEngine:engine];
    [view setWantsLayer:YES];
    view.layer.backgroundColor = [[NSColor yellowColor] CGColor];
    [self.window.contentView addSubview:view];
    [self.window makeFirstResponder:view];
}

@end

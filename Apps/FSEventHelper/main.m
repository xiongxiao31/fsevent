#import <Foundation/Foundation.h>
#import "FSEventCore.h"

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        fsevent_core_configuration configuration = {0};
        configuration.enable_http_server = 0;
        return fsevent_core_run(&configuration);
    }
}

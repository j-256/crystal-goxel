// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include "../ext_src/nfd/nfd_cocoa.m"

int main(void)
{
    @autoreleasepool {
        const nfdnfilteritem_t filters[] = {{"Applications", "app"}};
        NSArray *types = BuildAllowedContentTypes(filters, 1);
        if (![UTTypeApplicationBundle conformsToType:types[0]]) {
            fprintf(stderr, "Mac application filter rejects application bundles\n");
            return 1;
        }
        const nfdnfilteritem_t mixed[] = {{"Application or project", "app,json"}};
        types = BuildAllowedContentTypes(mixed, 1);
        if ([types count] != 2 ||
            ![UTTypeApplicationBundle conformsToType:types[0]] ||
            ![UTTypeJSON conformsToType:types[1]]) {
            fprintf(stderr, "Mixed filters lost bundle or JSON selection\n");
            return 1;
        }
        puts("Mac application bundle and ordinary file filters passed");
    }
    return 0;
}

// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real Objective-C wrapper dispatch; the container/keyplane consumer is modeled.
#include <objc/runtime.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long NSInteger;
typedef double CGFloat;
typedef struct CGSize { CGFloat width, height; } CGSize;
typedef struct CGPoint { CGFloat x, y; } CGPoint;
typedef struct CGRect { CGPoint origin; CGSize size; } CGRect;
static CGSize stockSize = {320,216};
static CGRect pixels = {{0,0},{1080,2400}}, points = {{0,0},{360,800}};
static double scale = 3, nativeScale = 3, width = 360;
static NSInteger idiom;
static BOOL floating, split;
static unsigned calls, installs, unavailable, applied;
static id expectedMode, expectedTraits;
static NSInteger expectedType;
__attribute__((objc_root_class))
@interface Fixture { Class isa; }
- (BOOL)isKindOfClass:(Class)cls;
@end
@implementation Fixture
- (BOOL)isKindOfClass:(Class)cls {
    for(Class c=object_getClass(self);c;c=class_getSuperclass(c)) if(c==cls)return YES;
    return NO;
}
@end
@interface UIScreen : Fixture
+ (UIScreen *)mainScreen;
- (CGRect)nativeBounds;
- (CGRect)bounds;
- (CGFloat)scale;
- (CGFloat)nativeScale;
@end
@implementation UIScreen
+ (UIScreen *)mainScreen {static UIScreen *s;if(!s)s=class_createInstance(self,0);return s;}
- (CGRect)nativeBounds {return pixels;}
- (CGRect)bounds {return points;}
- (CGFloat)scale {return scale;}
- (CGFloat)nativeScale {return nativeScale;}
@end
@interface UIKBScreenTraits : Fixture
- (NSInteger)idiom;
- (BOOL)isFloating;
- (BOOL)isSplit;
- (CGFloat)keyboardWidth;
@end
@implementation UIKBScreenTraits
- (NSInteger)idiom {return idiom;}
- (BOOL)isFloating {return floating;}
- (BOOL)isSplit {return split;}
- (CGFloat)keyboardWidth {return width;}
@end
@interface UIKeyboardLayoutStar : Fixture
+ (CGSize)keyboardSizeForInputMode:(id)mode screenTraits:(id)traits keyboardType:(NSInteger)type;
@end
@implementation UIKeyboardLayoutStar
+ (CGSize)keyboardSizeForInputMode:(id)mode screenTraits:(id)traits keyboardType:(NSInteger)type {
    assert(mode==expectedMode && traits==expectedTraits && type==expectedType);++calls;return stockSize;
}
@end
static void picasso_keyboard_note(const char *reason) {
    if(!strcmp(reason,"installed"))++installs;
    else if(!strcmp(reason,"unavailable"))++unavailable;
    else {assert(!strcmp(reason,"applied_320_to_360"));++applied;}
}
// GNU copies trailing argument offsets; Darwin omits them.
static char *normalize(char *s) {
    if(s) {size_t n=strlen(s);while(n && s[n-1]>='0' && s[n-1]<='9')s[--n]=0;}
    return s;
}
static char *retType(Method m) {return normalize(method_copyReturnType(m));}
static char *argType(Method m,unsigned i) {return normalize(method_copyArgumentType(m,i));}
static Class keyboardOverride, traitsOverride;
static Class lookup(const char *name) {
    if(keyboardOverride && !strcmp(name,"UIKeyboardLayoutStar"))return keyboardOverride;
    if(traitsOverride && !strcmp(name,"UIKBScreenTraits"))return traitsOverride;
    return objc_getClass(name);
}
#define objc_getClass lookup
#define method_copyReturnType retType
#define method_copyArgumentType argType
#include "keyboard-wrapper.inc"
#undef method_copyReturnType
#undef method_copyArgumentType
#undef objc_getClass
static CGSize query(void) {
    unsigned before=calls;
    CGSize result=[UIKeyboardLayoutStar keyboardSizeForInputMode:expectedMode screenTraits:expectedTraits keyboardType:expectedType];
    assert(calls==before+1);return result;
}
static void unchanged(void) {
    unsigned before=applied;CGSize result=query();
    assert(memcmp(&result,&stockSize,sizeof(result))==0 && applied==before);
}
// Model the observed caller: recenter using the reported size; a mismatch
// during a plane change drives setFrame -> updateLayout -> default letters.
static void consumer(CGSize size, BOOL *numbers, double *origin) {
    *origin=(360-size.width)/2;
    if(size.width!=360)*numbers=NO;
}
int main(int argc,char **argv) {
    Class cls=objc_getClass("UIKeyboardLayoutStar");
    SEL sel=sel_registerName("keyboardSizeForInputMode:screenTraits:keyboardType:");
    Method m=class_getClassMethod(cls,sel);IMP original=method_getImplementation(m);
    uintptr_t base=(uintptr_t)original-0x399c0c;
    if(argc>1) {
        if(!strcmp(argv[1],"getter")) {
            Class t=objc_getClass("UIKBScreenTraits");SEL g=sel_registerName("keyboardWidth");
            Method gm=class_getInstanceMethod(t,g);
            traitsOverride=objc_allocateClassPair(t,"BadKeyboardTraits",0);
            assert(class_addMethod(traitsOverride,g,method_getImplementation(gm),"q16@0:8"));
            objc_registerClassPair(traitsOverride);
        } else {
            const char *type=!strcmp(argv[1],"return")?"q40@0:8@16@24q32":
                !strcmp(argv[1],"args")?"{CGSize=dd}32@0:8@16@24":"{CGSize=dd}40@0:8@16@24d32";
            keyboardOverride=objc_allocateClassPair(objc_getClass("Fixture"),"BadKeyboardLayout",0);
            assert(class_addMethod(object_getClass(keyboardOverride),sel,original,type));
            objc_registerClassPair(keyboardOverride);
        }
        picasso_keyboard_install(base);assert(!installs && unavailable==1);
        assert(method_getImplementation(class_getClassMethod(cls,sel))==original);
        puts("PASS incompatible ABI preserved");return 0;
    }
    expectedMode=class_createInstance(objc_getClass("Fixture"),0);
    expectedTraits=class_createInstance(objc_getClass("UIKBScreenTraits"),0);expectedType=7;
    BOOL numbers=YES;double origin=0;consumer(query(),&numbers,&origin);
    assert(!numbers && origin==20);
    picasso_keyboard_install(base+4);assert(!installs && unavailable==1);
    picasso_keyboard_install(base);assert(installs==1 && picasso_keyboard_original==(PicassoKeyboardSize)original);
    for(unsigned i=0;i<100;i++)picasso_keyboard_install(base);
    assert(installs==1);
    for(unsigned i=0;i<10000;i++) {
        numbers=YES;CGSize got=query();consumer(got,&numbers,&origin);
        assert(numbers && origin==0 && got.height==stockSize.height);
    }
    idiom=1;unchanged();idiom=0;
    floating=YES;unchanged();floating=NO;
    split=YES;unchanged();split=NO;
    width=375;unchanged();width=360;
    points.size=(CGSize){800,360};unchanged();points.size=(CGSize){360,800};
    pixels.size.width=1170;unchanged();pixels.size.width=1080;
    scale=2;unchanged();scale=3;
    nativeScale=2;unchanged();nativeScale=3;
    stockSize.width=360;unchanged();stockSize.width=320;
    for(unsigned i=0;i<5;i++){double heights[]={0,-1,400,INFINITY,NAN};stockSize.height=heights[i];unchanged();}
    stockSize.height=233;assert(query().height==233);
    id saved=expectedTraits;expectedTraits=nil;unchanged();expectedTraits=expectedMode;unchanged();expectedTraits=saved;
    object_dispose(expectedMode);object_dispose(expectedTraits);
    puts("PASS actual wrapper: stable 360-point container, modeled numeric-plane retention, exclusions, ABI and idempotent installation");
}

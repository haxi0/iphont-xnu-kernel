#!/usr/bin/env python3
"""Check bluetoothd integration guards and compile the driver's actual H4 parser on the host."""
import pathlib
import re
import runpy
import plistlib
import struct
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
binary = (root / 'ios165/rootfs/usr/sbin/bluetoothd').read_bytes()
source = (root / 'userland/src/inject_bt.c').read_text()
commands = struct.unpack_from('<I', binary, 16)[0]
offset = 32
uuid = None
for _ in range(commands):
    cmd, size = struct.unpack_from('<II', binary, offset)
    assert size >= 8 and offset + size <= len(binary)
    if cmd == 0x1b:
        uuid = binary[offset + 8:offset + 24]
    offset += size
expected = source.split('bluetoothd_uuid[16] = {')[1].split('}')[0]
assert uuid == bytes(int(n, 16) for n in re.findall(r'0x([0-9a-f]+)', expected))
constants = {k: int(v, 16) for k, v in re.findall(r'#define (BT_\w+) (0x[0-9a-fA-F]+)', source)}
base = 0x100000000
# Chained rebase targets in this non-arm64e Mach-O store the image offset in the low 36 bits.
def target(address):
    raw = struct.unpack_from('<Q', binary, address - base)[0]
    return base + (raw & 0xfffffffff) if raw else 0
assert target(constants['BT_INIT_SLOT']) == constants['BT_REPORT_STEP']
assert target(constants['BT_INIT_SLOT'] + 8) == 0
assert target(constants['BT_FACTORY_SLOT']) == constants['BT_FACTORY_INVOKE']
assert target(constants['BT_PERIODIC_SLOT']) == constants['BT_PERIODIC_STEP']
for name, opcode in [('BT_VENDOR_GET', 0xa9bf7bfd), ('BT_REPORT_STEP', 0xd100c3ff),
                     ('BT_PERIODIC_STEP', 0xd100c3ff), ('BT_INIT_ADVANCE', 0xa9be4ff4)]:
    assert struct.unpack_from('<I', binary, constants[name] - base)[0] == opcode
# Continue-one-step takes x0 into x19; the finish-with-status function instead takes x1.
assert struct.unpack_from('<I', binary, constants['BT_INIT_ADVANCE'] - base + 12)[0] == 0xaa0003f3
print('PASS: bluetoothd UUID, init-table terminator, and function guards')

selection = runpy.run_path(str(root / 'ios165/select-system.py'), run_name='bt_selection_check')
hal = 'System/Library/Audio/Plug-Ins/HAL/'
assert selection['wanted_file'](hal + 'BTAudioHALPlugin.driver/BTAudioHALPlugin', 562992)
assert selection['wanted_file'](hal + 'BTAudioHALPlugin.driver/Info.plist', 1207)
assert not selection['wanted_file'](hal + 'AppleAOPAudioPlugin.driver/AppleAOPAudioPlugin', 1000)
print('PASS: Bluetooth HAL selected without enabling unrelated Apple hardware plug-ins')
if (root / 'userland/root/LaunchDaemons/com.hackios.bluetool.plist').exists():
    bundle = root / 'userland/root/hal/BTAudioHALPlugin.driver'
    info = plistlib.loads((bundle / 'Info.plist').read_bytes())
    assert info['CFBundleIdentifier'] == 'com.apple.audio.BTAudioHALPlugin'
    assert (bundle / info['CFBundleExecutable']).read_bytes() == (
        root / 'ios165/rootfs' / hal / 'BTAudioHALPlugin.driver' / info['CFBundleExecutable']).read_bytes()
    assert (root / 'userland/root/hal/HKAudio.driver').is_symlink()
    print('PASS: staged native Bluetooth HAL matches original; HKAudio retained')


kernel = (root / 'xnu-src/xnu-8796.121.2/bsd/dev/hk_bt.c').read_text()
assert all(name not in kernel for name in ('complete_here', 'adapt_event', 'adapt_command'))
parser = kernel[kernel.index('struct h4 {'):kernel.index('static void\ntrace(')]
parser = parser.replace('static struct h4 trace_in, trace_out;', 'struct h4 trace_in, trace_out;')
nvm = kernel[kernel.index('static bool\nnvm_fix('):kernel.index('static const uint8_t *\nfirmware(')]
sco = kernel[kernel.index('static uint32_t bt_sco_hci'):kernel.index('static uint8_t send_packet[')]
audio = (root / 'userland/src/sound_test.c').read_text()
audio = audio[audio.index('static double bt_peak'):audio.index("/* CoreHaptics' constants")]

assert 'ok = nvm_fix(nvm, nvm_size) && download(' in kernel
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define say(...) ((void)0)
'''+parser+r'''
static uint8_t address[6] = {1,2,3,4,5,6};
#define QCA_BAUD_3000000 14
'''+nvm+r'''
#define EV_COMMAND_STATUS 0x0f
#define os_atomic_inc(p, m) (++*(p))
#define os_atomic_dec(p, m) (--*(p))
#define os_atomic_load(p, m) (*(p))
#define LCK_MTX_DECLARE(name, group) static int name
#define lck_mtx_lock(m) ((void)(m))
#define lck_mtx_unlock(m) ((void)(m))
static int hk_bt_lck_grp;
static void trace(char d, uint8_t type, const uint8_t *data, uint32_t length) { (void)d;(void)type;(void)data;(void)length; }
static uint8_t wrote[16][260]; static unsigned writes, wrote_size[16];
static int host_write(const uint8_t *b, uint32_t n) { assert(writes<16 && n<=260); memcpy(wrote[writes],b,n); wrote_size[writes++]=n; return 0; }
'''+sco+r'''
#include <math.h>
#include <stdarg.h>
typedef void *id;
typedef const char *SEL;
typedef unsigned char Boolean;
typedef double CFAbsoluteTime;
#define NULL_ID ((id)(uintptr_t)1)
#define CFSTR(s) (s)
#define kCFRunLoopDefaultMode 0
static double peak=0.5, test_clock;
static unsigned tones;
static int session_ok=1;
static const char *port_type="Speaker", *port_name="Speaker";
static SEL sel_registerName(const char *s) { return s; }
static id objc_getClass(const char *s) { (void)s; return NULL_ID; }
static uintptr_t objc_msgSend(id object, SEL sel, ...) {
    (void)object;
    if(!strcmp(sel,"count")) return 1;
    if(!strcmp(sel,"objectAtIndex:")) return (uintptr_t)NULL_ID;
    return session_ok;
}
static id send(id object,const char *s) {
    (void)object;
    if(!strcmp(s,"portType")) return (id)port_type;
    if(!strcmp(s,"portName")) return (id)port_name;
    if(!strcmp(s,"description")) return NULL;
    return NULL_ID;
}
static const char *text(id s) { return s?(const char *)s:"none"; }
static CFAbsoluteTime CFAbsoluteTimeGetCurrent(void) { return test_clock; }
static void CFRunLoopRunInMode(int mode,double delay,int again) {
    (void)mode;(void)again;test_clock+=delay;
}
static void route(void) {}
static int tone(double duration) { assert(duration==2 && peak==0.01);tones++;return 0; }
'''+audio+r'''
int main(void) {
    assert(bluetooth_tone(0)==1 && bluetooth_tone(NAN)==1 && bluetooth_tone(301)==1);
    assert(tones==0);
    assert(bluetooth_tone(1)==1 && tones==0); /* speaker must stay silent */
    port_type="BluetoothA2DPOutput";port_name="Another headset";
    assert(bluetooth_tone(1)==1 && tones==0);
    port_name="CMF Buds Pro 2";
    assert(bluetooth_tone(1)==0 && tones==1 && peak==0.5);
    session_ok=0;
    assert(bluetooth_tone(1)==1 && tones==1);
    puts("PASS: actual quiet-tone gate rejects other routes/session failures; target is 2 s at 1 percent peak");
    uint8_t tiny[4]={2,0,0,0};
    for(unsigned n=0;n<4;n++) assert(!nvm_fix(tiny,n));
    uint8_t tags[50]={2,46,0,0, 2,0,6,0};
    tags[22]=17; tags[24]=3; tags[37]=27; tags[39]=1;
    assert(nvm_fix(tags,sizeof(tags)));
    assert(!memcmp(tags+16,(uint8_t[]){6,5,4,3,2,1},6));
    assert(tags[34]==0x80 && tags[35]==14 && tags[49]==1);
    tags[24]=255; assert(!nvm_fix(tags,sizeof(tags)));
    uint8_t truncated[5]={2,1,0,0,0}; assert(!nvm_fix(truncated,5));
    tiny[1]=10; assert(!nvm_fix(tiny,4));
    tiny[0]=1; tiny[1]=0; assert(!nvm_fix(tiny,4));
    /* the Setup Synchronous Connection bluetoothd sent in 002629 (CVSD, eSCO), with its H4 type */
    uint8_t cmd[64]={1,0x28,0x04,17, 2,0, 0x40,0x1f,0,0, 0x40,0x1f,0,0, 0x1a,0, 0x60,0, 2, 0xbe,2};
    assert(sco_enhance(cmd,21)==4+59 && cmd[1]==0x3d && cmd[2]==0x04 && cmd[3]==59 && sco_setups==1);
    assert(cmd[4]==2 && cmd[5]==0 && cmd[6]==0x40 && cmd[10]==0x40);      /* handle, bandwidths kept */
    assert(cmd[14]==2 && cmd[19]==2 && cmd[24]==60 && cmd[26]==60);       /* CVSD on air, 60-byte frames */
    assert(cmd[28]==0x80 && cmd[29]==0x3e && cmd[36]==4 && cmd[41]==4);   /* 16000 B/s linear PCM from the host */
    assert(cmd[46]==16 && cmd[48]==16 && cmd[50]==2 && cmd[51]==2 && cmd[52]==0 && cmd[53]==0);
    assert(cmd[54]==0 && cmd[55]==0 && cmd[56]==16 && cmd[57]==16);       /* data paths HCI, 16-bit units */
    assert(cmd[58]==0x1a && cmd[59]==0 && cmd[60]==0xbe && cmd[61]==2 && cmd[62]==2);
    uint8_t status[6]={0x0f,4,0,1,0x3d,0x04};
    sco_event(status,6); assert(status[4]==0x28 && status[5]==0x04 && sco_setups==0);
    status[4]=0x3d; sco_event(status,6); assert(status[4]==0x3d);          /* nothing pending: untouched */
    uint8_t acc[68]={1,0x29,0x04,21, 1,2,3,4,5,6, 0x40,0x1f,0,0, 0x40,0x1f,0,0, 0x0d,0, 0x63,0, 1, 0x3f,3};
    assert(sco_enhance(acc,25)==4+63 && acc[1]==0x3e && acc[3]==63 && sco_accepts==1);
    assert(acc[4]==1 && acc[9]==6 && acc[18]==3 && acc[40]==3 && acc[58]==0 && acc[59]==0 && acc[60]==1);
    assert(acc[62]==0x0d && acc[64]==0x3f && acc[65]==3 && acc[66]==1);
    uint8_t st2[6]={0x0f,4,0x0c,1,0x3e,0x04}; sco_event(st2,6); assert(st2[4]==0x29 && sco_accepts==0);
    uint8_t alaw[21]={1,0x28,0x04,17, 2,0, 0,0,0,0, 0,0,0,0, 0,0, 0x62,0, 0, 0,0};
    assert(sco_enhance(alaw,21)==21 && alaw[1]==0x28);                       /* other voice settings as they came */
    uint8_t other[4]={1,0x03,0x0c,0}; assert(sco_enhance(other,4)==4 && other[1]==0x03);
    bt_sco_hci=0; cmd[1]=0x28; cmd[3]=17; assert(sco_enhance(cmd,21)==21 && cmd[1]==0x28 && sco_setups==0);
    /* the pace: iOS's bytes go in packets of the chip's size, at most SCO_AHEAD ahead of the chip's own */
    uint8_t up[17]={0x2c,15,0}; sco_event(up,17); assert(sco_credit==2 && sco_have==0);
    uint8_t sco[3+120]={3,0,120}; for(int i=0;i<120;i++) sco[3+i]=(uint8_t)i;
    for(int k=0;k<4;k++) sco_put(sco,123);
    assert(sco_flush()==0 && writes==2 && wrote_size[0]==124 && wrote[0][0]==3 && wrote[0][1]==3 && wrote[0][3]==120);
    assert(wrote[1][4]==0 && wrote[1][123]==119 && sco_have==240 && sco_credit==0);
    uint8_t chip[3+60]={3,0,60}; sco_chip_sent(chip,63); assert(sco_unit==60 && sco_credit==1);
    assert(sco_flush()==0 && writes==3 && wrote_size[2]==64 && wrote[2][3]==60 && sco_have==180);
    uint8_t small[3+8]={3,0,8}; sco_put(small,11); assert(sco_have==188);
    for(int k=0;k<20;k++) sco_put(sco,123); assert(sco_have==SCO_QUEUE && sco_dropped>0);   /* full: the oldest dropped */
    sco_chip_sent(chip,63); sco_chip_sent(chip,63); sco_chip_sent(chip,63); assert(sco_credit==2);
    writes=0; assert(sco_flush()==0 && writes==2);
    puts("PASS: actual SCO pace: chip-sized packets, two ahead at most, bounded queue");
    puts("PASS: actual SCO translation: legacy set-up/accept as enhanced with the HCI data path, status opcode restored");
    puts("PASS: actual NVM parser, address/baud/sleep tags and malformed input rejection");
    const uint8_t packets[][10] = {
        {1,3,12,0}, {2,1,0,3,0,9,8,7}, {3,1,0,1,6}, {4,14,4,1,3,12,0}, {5,1,0,1,0,7}
    };
    const unsigned sizes[] = {4,8,5,7,6};
    for (unsigned k=0;k<5;k++) {
        for (unsigned split=0;split<=sizes[k];split++) {
            struct h4 p={0}; unsigned complete=0;
            for(unsigned part=0;part<2;part++) {
                unsigned begin=part?split:0, end=part?sizes[k]:split;
                for(unsigned j=begin;j<end;j++) if(h4_feed(&p,packets[k][j])) {
                    assert(j+1==sizes[k]); assert(!p.overlong);
                    assert(p.have==sizes[k]-1); assert(!memcmp(p.data,packets[k]+1,p.have));
                    complete++; h4_done(&p);
                }
            }
            assert(complete==1 && p.type==0);
        }
    }
    struct h4 p={0};
    uint8_t header[]={2,1,0,1,8}; /* 2049-byte ACL body: consume, reject, preserve next packet boundary */
    for(unsigned j=0;j<sizeof(header);j++) assert(!h4_feed(&p,header[j]));
    for(unsigned j=0;j<2049;j++) assert(h4_feed(&p,0)==(j==2048));
    assert(p.overlong); h4_done(&p);
    for(unsigned j=0;j<4;j++) assert(h4_feed(&p,packets[0][j])==(j==3));
    assert(!p.overlong);
    puts("PASS: actual H4 parser, all split boundaries, five types, oversized-packet recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='hackios-bt-') as tmp:
    path = pathlib.Path(tmp)
    (path / 'check.c').write_text(harness)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined', '-fno-sanitize=function',
                    str(path / 'check.c'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)

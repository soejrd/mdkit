#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
"""Standalone TRX contract suite. Compiles selected standalone voices.
Run: python3 tests/trx/family/run.py [--source-root src/trx] [--voices bd,sd,...]
Source root contains <voice>/mdkit_trx_<voice>.{c,h}. Outputs default to out/trx-family.
"""
import argparse,subprocess,os
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=ROOT/'src/trx');p.add_argument('--voices',default=None);p.add_argument('--out',type=Path,default=ROOT/'out/trx-family');p.add_argument('--no-wasm',action='store_true');a=p.parse_args();a.voices=a.voices or ','.join(p.parent.name for p in sorted(a.source_root.glob('*/voice.mk')));a.out.mkdir(parents=True,exist_ok=True)
cc=os.environ.get('CC','cc');clang=os.environ.get('WASM_CC','/opt/homebrew/opt/llvm/bin/clang');node=os.environ.get('NODE','node');here=Path(__file__).resolve().parent;fail=[];objects=[];decl=[];refs=[]
def run(cmd,label):
 r=subprocess.run([str(x) for x in cmd],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);print(r.stdout,end='');
 if r.returncode:fail.append(label)
 return r.returncode==0
for name in a.voices.split(','):
 src=a.source_root/name/f'mdkit_trx_{name}.c';header=f'mdkit_trx_{name}.h';prefix=f'mdkit_trx_{name}';sym=f'mdkit_voice_trx_{name}';mid=16+['bd','sd','xt','cp','rs','cb','ch','oh','cy','ma','cl','xc','b2'].index(name)
 print(f'=== {name} ===',flush=True);dest=a.out/name;dest.mkdir(exist_ok=True);obj=dest/'voice.o';base=['-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),'-I'+str(src.parent)];defs=[f'-DMDKIT_TEST_VOICE_HEADER="{header}"',f'-DVOICE_SYMBOL={sym}',f'-DVOICE_MACHINE_ID={mid}']
 if not run([cc,*base,'-c',src,'-o',obj],name+' compile'):continue
 objects.append(obj);decl.append(f'extern const mdkit_voice {sym};');refs.append('&'+sym)
 exe=dest/'test';raw=dest/'native.raw'
 if run([cc,*base,*defs,here/'test_voice.c',obj,'-lm','-o',exe],name+' test compile'):run([exe,'--render',raw],name+' native checks')
 if a.no_wasm:continue
 wasm=dest/'voice.wasm';exports=['state_size','init','set_control','trigger','render','sample_rate'];cmd=[clang,'--target=wasm32','-std=c99','-O2','-nostdlib','-ffreestanding','-Wl,--no-entry','-Wl,--strip-all',*[f'-Wl,--export={prefix}_{e}' for e in exports],'-I'+str(ROOT/'include'),'-I'+str(src.parent),f'-DMDKIT_EXAMPLE_VOICE_HEADER="{header}"',f'-DVOICE_SYMBOL={sym}',f'-DVOICE_PREFIX={prefix}',ROOT/'website/wasm_bridge.c',src,'-o',wasm]
 if run(cmd,name+' wasm compile') and raw.exists():run([node,here/'check_parity.cjs',wasm,raw,prefix],name+' native/WASM parity')
if objects:
 main=a.out/'link_all.c';main.write_text('#include "mdkit.h"\n'+ '\n'.join(decl)+'\nint main(void){const mdkit_voice *v[]={'+','.join(refs)+'};for(unsigned i=0;i<sizeof(v)/sizeof(v[0]);++i)if(!v[i]->render||!v[i]->state_bytes)return 1;return 0;}\n')
 if run([cc,'-std=c99','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),main,*objects,'-o',a.out/'link_all'],'all together link'):run([a.out/'link_all'],'all together descriptor check')
print('FAILURES: '+(', '.join(fail) if fail else 'none'));raise SystemExit(bool(fail))

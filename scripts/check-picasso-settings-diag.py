"""Check extracted settings diagnostics with host lock/map models, not hardware."""
import argparse, hashlib, json, re, shutil, subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--xnu',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    sources={n:(a.xnu/n).read_text() for n in ['bsd/kern/kern_exit.c','osfmk/kern/task.c','bsd/dev/picasso_usb.c']}
    task=re.findall(r'(/\* PICASSO_SETTINGS_DIAG_TASK_BEGIN.*?/\* PICASSO_SETTINGS_DIAG_TASK_END \*/)',sources['osfmk/kern/task.c'],re.S)
    assert len(task)==1
    (a.output/'task.inc').write_text(task[0])
    bsd=sources['bsd/kern/kern_exit.c'].split('/* PICASSO_SETTINGS_DIAG_BSD_BEGIN */')[1]
    (a.output/'bsd.inc').write_text('/* PICASSO_SETTINGS_DIAG_BSD_BEGIN */'+bsd.split('/* PICASSO_SETTINGS_DIAG_BSD_END */')[0]+'/* PICASSO_SETTINGS_DIAG_BSD_END */')
    locked=bsd.split('proc_list_lock();')[1].split('proc_list_unlock();')[0]
    assert all(n not in locked for n in ['printf(','task_lock','picasso_settings_task_snapshot('])
    assert 'settings_diag_sample >= 8' in bsd and '5ULL * NSEC_PER_SEC' in bsd and 'thread_call_enter(settings_diag_call)' in bsd
    usb=sources['bsd/dev/picasso_usb.c'];poll=usb[usb.index('\nud_poll_serialized(void)'):]
    assert poll.index('os_atomic_store(&ud.poll_lock, 0, release);')<poll.index('picasso_settings_diag_request();')
    assert 'settings_diag_pending, 0, acquire) && normal' in poll
    vm=(a.xnu/'osfmk/vm/vm_fault.c').read_text()
    assert 'return KERN_FAILURE;' in vm.split('if (get_preemption_level() != 0)',1)[1].split('\n\t}',1)[0]
    report=dict(passed=False,scope=__doc__,cases=30,runs=[],source_sha256={n:hashlib.sha256(s.encode()).hexdigest() for n,s in sources.items()})
    try:
        for group,cases in [('task',20),('bsd',10)]:
            fixture=Path(__file__).parent/('tests/picasso-settings-diag-'+group+'.c')
            if not fixture.is_file():fixture=Path(__file__).parent/('test-settings-'+group+'.c')
            shutil.copyfile(fixture,a.output/(group+'.c'))
            for sanitized in [False,True]:
                name=group+('-asan' if sanitized else '')
                cmd=[shutil.which('clang'),'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function',str(a.output/(group+'.c')),'-o',str(a.output/name)]
                if sanitized:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
                r=subprocess.run(cmd,text=True,capture_output=True);(a.output/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
                entry=dict(group=group,sanitized=sanitized,passed_cases=[]);report['runs'].append(entry)
                with (a.output/(name+'-run.log')).open('w') as log:
                    for i in range(cases):
                        r=subprocess.run([str(a.output/name),str(i)],stdout=log,stderr=subprocess.STDOUT);r.check_returncode();entry['passed_cases'].append(i)
        report['passed']=True
    finally:(a.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS 30 normal + 30 ASan/UBSan worker/lifecycle/locks/maps/frames cases')

if __name__=='__main__':main()

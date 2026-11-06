#!/usr/bin/env python3
"""Exercise Compose acquisition decisions without network or device writes."""
import argparse,base64,hashlib,importlib.util,json,sys,zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    spec=importlib.util.spec_from_file_location('inputs',ROOT/'scripts/picasso_linux_inputs.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    script=(ROOT/'scripts/picasso_linux_inputs.py').read_bytes();revision='a'*40
    archive=b'fixture archive';cases=[];candidateRecipe='recipe';corrupt=False;expired=False;producer=True
    artifact_name='picasso-bootimg-123'
    def api(repository,path):
        if '/runs?' in path:return {'workflow_runs':[dict(id=123,head_sha=revision,head_branch='feat/ios16-gpu',conclusion='success',event='workflow_dispatch',path='.github/workflows/compose-bootimg.yml')]} if producer else {'workflow_runs':[]}
        if path.startswith('contents/'):return {'content':base64.b64encode(script).decode()}
        if path.endswith('/artifacts?per_page=100'):return {'artifacts':[dict(id=456,name=artifact_name,expired=expired,digest='sha256:'+hashlib.sha256(archive).hexdigest(),size_in_bytes=len(archive))]}
        raise AssertionError(path)
    def download(*args,stdout,**kwargs):stdout.write(archive);return SimpleNamespace(returncode=0)
    def extract(archive,staged):
        staged.mkdir();(staged/'manifest.json').write_text(json.dumps({'recipe_sha256':candidateRecipe}))
    def validate(staged,tree):
        if corrupt:raise ValueError('Compose input hash mismatch: setup.dbr')
        return {}
    def attempt(name,error=None):
        try:module.acquire('owner/repo','feat/ios16-gpu',out,out/name)
        except ValueError as e:assert error and error in str(e),(name,str(e))
        else:assert error is None,name
        cases.append(name)
    with patch.object(module,'api',api),patch.object(module,'recipe',lambda t:'recipe'),patch.object(module,'extract',extract),patch.object(module,'validate',validate),patch.object(module.subprocess,'run',download):
        attempt('compatible')
        proof=json.loads((out/'compatible/reuse.json').read_text());assert proof['producer_run_id']==123 and proof['artifact_id']==456
        artifact_name='picasso-data-seed-123';attempt('dedicated-seed')
        corrupt=True;attempt('corrupt','hash mismatch');corrupt=False
        candidateRecipe='old';attempt('incompatible','No compatible');candidateRecipe='recipe'
        expired=True;attempt('expired','No compatible');expired=False
        producer=False;attempt('absent','No compatible')
    for prefix in ('','linux-inputs/','out/picasso/linux-inputs/'):
        name='archive-'+str(len(cases));archive_path=out/(name+'.zip')
        with zipfile.ZipFile(archive_path,'w') as z:
            for f in (*module.FILES,'manifest.json'):z.writestr(prefix+f,b'fixture')
        module.extract(archive_path,out/name)
        assert set(p.name for p in (out/name).iterdir())=={*module.FILES,'manifest.json'}
        cases.append(name)
    missing='No compatible iOS16 Compose input bundle is retained.'
    for allow,message,success in ((False,missing,False),(True,missing,True),
                                 (True,'Compose input hash mismatch',False)):
        argv=['inputs','reuse-optional','--tree',str(out),'--output',str(out/'unused')]
        if allow:argv+=['--allow-new-seed']
        with patch.object(sys,'argv',argv),patch.object(module,'reuse',side_effect=ValueError(message)):
            try:module.main()
            except ValueError:assert not success
            else:assert success
        cases.append(f'new-seed-{allow}-{message}')
    (out/'results.json').write_text(json.dumps(dict(passed=True,cases=cases,network=False),indent=2)+'\n')
    print('PASS: exact seed acquisition, corrupt input refusal and missing/expired recipes')
if __name__=='__main__':main()

#!/usr/bin/env python3
from pathlib import Path
import argparse,importlib.util,json,struct
from unicorn import UC_HOOK_INTR
from unicorn.arm64_const import UC_ARM64_REG_NZCV,UC_ARM64_REG_PC
R=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser(description='Execute the signed ARM64 Skywalk ABI used by bluetoothd')
p.add_argument('--binary',type=Path,required=True);p.add_argument('--library',type=Path,required=True)
p.add_argument('--layout',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
s=importlib.util.spec_from_file_location('native',R/'drivers/PicassoBluetooth/tests/native-advertising.py');native=importlib.util.module_from_spec(s);s.loader.exec_module(native)
L=json.loads(a.layout.read_text())
binary=a.binary.read_bytes();library=a.library.read_bytes()
class Trap(Exception):pass
class Skywalk(native.NativeAdvertising):
 def __init__(self,version=18):
  self.heap=0x500000000;self.err=0x600000000;self.base=0x700000000;self.allocations=[];self.closed=[];self.syscalls=[];self.channel_error=0
  super().__init__(binary,library)
  for address,(kind,name) in self.main.imports.items():
   if kind==8 and name.startswith(('_os_channel','_os_packet','_os_buflet')):self.hooks.pop(address,None)
  for addr,size in [(self.heap,0x20000),(self.err,0x1000),(self.base,0x20000)]:self.uc.mem_map(addr,size)
  self.uc.hook_add(UC_HOOK_INTR,self.interrupt)
  self.info=bytearray(L['info.size'])
  def info(field,value,width=4):struct.pack_into('<'+('Q' if width==8 else 'I'),self.info,L['info.'+field],value)
  info('cinfo_mem_base',self.base,8);info('cinfo_mem_map_size',0x20000,8);info('cinfo_nxprov_params',0)
  struct.pack_into('<I',self.info,L['info.cinfo_nxprov_params']+L['params.nxp_max_frags'],1)
  self.put(self.base,'schema','csm_ver',version);self.put(self.base,'schema','csm_flags',L['CSM_ACTIVE']);self.put(self.base,'schema','csm_tx_rings',1);self.put(self.base,'schema','csm_rx_rings',1)
  self.put(self.base,'schema','csm_md_type',2);self.put(self.base,'schema','csm_md_subtype',1);self.cookie=0xaabbccdd11223344;self.put(self.base,'schema','csm_md_redzone_cookie',self.cookie,8)
  self.rings=[];self.slots=[];self.packets=[];self.buffers=[]
  for i in range(2):
   ring=self.base+0x1000+i*0x1000;slot=self.base+0x4000+i*0x1000;metadata=self.base+0x6000+i*0x2000;buf=self.base+0xa000+i*0x4000
   self.write64(self.base+L['schema.csm_ring_ofs']+i*16,ring-self.base);self.write64(self.base+L['schema.csm_ring_ofs']+i*16+8,slot-self.base)
   for field,val,width in [('ring_tail',2,4),('ring_num_slots',4,4),('ring_def_buf_size',1024,4),('ring_md_size',176,2),('ring_kind',1-i,2),('ring_md_base',metadata-ring,8),('ring_def_buf_base',buf-ring,8),('ring_sd_base',slot-ring,8)]:self.put(ring,'ring',field,val,width)
   for index in range(4):
    self.uc.mem_write(slot+8*index,struct.pack('<IHH',index,1,0));pre=metadata+index*176;q=pre+16
    self.put(pre,'preamble','mdp_redzone',(q-metadata)^self.cookie,8);self.put(pre,'preamble','mdp_idx',index)
    self.put(q,'packet','pkt_bufs_max',1,2);self.put(q,'packet','pkt_bufs_cnt',1,2);b=q+L['packet.pkt_qum_buf']
    self.put(b,'buflet','buf_idx',index);self.put(b,'buflet','buf_nbft_idx',L['OBJ_IDX_NONE']);self.put(b,'buflet','buf_dlim',1024);self.put(b,'buflet','buf_dlen',7)
    self.put(q,'packet','pkt_qum.qum_len',7)
   self.rings.append(ring);self.slots.append(slot);self.packets.append(metadata+16);self.buffers.append(buf)
 def put(self,base,typ,field,value,width=4):self.uc.mem_write(base+L[typ+'.'+field],value.to_bytes(width,'little'))
 def interrupt(self,uc,number,unused):
  pc=self.uc.reg_read(UC_ARM64_REG_PC)
  if number!=2:raise Trap('validation trap')
  nr=self.x(16);self.syscalls.append((nr,tuple(self.x(i) for i in range(4))))
  result=self.channel_error
  if not result:
   if nr==510:result=20
   elif nr==511:self.uc.mem_write(self.x(1),bytes(self.info));result=0
   elif nr in (512,513,514):result=0
   else:raise AssertionError('unexpected syscall '+str(nr))
  self.uc.reg_write(UC_ARM64_REG_NZCV,0x20000000 if self.channel_error else 0)
  self.uc.reg_write(native.UC_ARM64_REG_X0,result)
 def import_call(self,name):
  if name in ('_malloc','_calloc'):
   n=self.x(0)*(self.x(1) if name=='_calloc' else 1);addr=self.heap+sum(self.allocations);self.allocations.append((n+15)&~15);self.uc.mem_write(addr,b'\0'*n);self.ret(addr)
  elif name=='_free':self.ret()
  elif name=='___error':self.ret(self.err)
  elif name=='_guarded_close_np':self.closed.append(self.x(0));self.ret()
  elif name in ('_vfprintf','_snprintf'):self.ret()
  else:super().import_call(name)
 def api(self,name,*args):return self.call(self.lib.symbols['_picasso_sw_'+name],*args)

t=Skywalk();entries=json.loads((R/'drivers/PicassoBluetooth/userland/skywalk-imports.json').read_text())
imports={name:addr for addr,(kind,name) in t.main.imports.items() if kind==8 and name.startswith(('_os_channel','_os_packet','_os_buflet'))}
assert set(imports)=={'_'+e['name'] for e in entries}
for e in entries:
 assert imports['_'+e['name']]==int(e['stub'],0)
 assert t.read(int(e['slot'],0),8)==t.lib.symbols['_picasso_sw_'+e['name']]
 # Branch through the actual offline patched import stub.
 if e['name']=='os_channel_attr_create':assert t.call(int(e['stub'],0))!=0
ch=t.call(0x1006609f0,t.ARENA+0x10000,0)
assert ch and [nr for nr,_ in t.syscalls]==[510,511]
tx=t.api('os_channel_tx_ring',ch,0);rx=t.api('os_channel_rx_ring',ch,0)
assert t.api('os_channel_get_fd',ch)==20 and tx and rx
assert t.api('os_channel_available_slot_count',tx)==3
prop=t.ARENA+0x18000;slot=t.api('os_channel_get_next_slot',tx,0,prop)
assert slot==t.slots[0] and t.read(prop+L['prop.sp_len'],2)==1024 and t.read(prop+L['prop.sp_buf_ptr'],8)==t.buffers[0]
packet=t.api('os_channel_slot_get_packet',tx,slot);b=t.api('os_packet_get_next_buflet',packet,0)
assert b==t.packets[0]+L['packet.pkt_qum_buf'] and t.api('os_buflet_get_data_limit',b)==1024
assert t.api('os_buflet_set_data_length',b,11)==0 and t.api('os_buflet_get_data_length',b)==11
assert t.api('os_buflet_set_data_length',b,1025)==0 and t.api('os_packet_finalize',packet)!=0
assert t.api('os_buflet_set_data_length',b,11)==0
assert t.api('os_packet_finalize',packet)==0 and t.read(t.packets[0]+L['packet.pkt_qum.qum_len'],4)==11
uuid=t.ARENA+0x19000;out=uuid+32;t.uc.mem_write(uuid,bytes(range(16)))
t.api('os_packet_set_flow_uuid',packet,uuid);t.api('os_packet_get_flow_uuid',packet,out);assert bytes(t.uc.mem_read(out,16))==bytes(range(16))
t.put(prop,'prop','sp_len',11,2);t.api('os_channel_set_slot_properties',tx,slot,prop);assert t.api('os_channel_advance_slot',tx,slot)==0
assert t.read(t.rings[0],4)==1 and t.api('os_channel_sync',ch,0)==0
rslot=t.api('os_channel_get_next_slot',rx,0,prop);assert rslot==t.slots[1] and t.read(prop+L['prop.sp_len'],2)==7
t.api('os_channel_destroy',ch);assert t.closed==[20]
for version in [15,17,19]:
 bad=Skywalk(version)
 try:bad.api('os_channel_create',bad.ARENA+0x10000,0);raise AssertionError('wrong version accepted')
 except Trap:pass
bad=Skywalk();bad.channel_error=6;assert bad.api('os_channel_create',bad.ARENA+0x10000,0)==0 and bad.read(bad.err,4)==6
bad=Skywalk();ch=bad.api('os_channel_create',bad.ARENA+0x10000,0);tx=bad.api('os_channel_tx_ring',ch,0);bad.write64(bad.packets[0]-16,0)
try:bad.api('os_channel_get_next_slot',tx,0,bad.ARENA+0x18000);raise AssertionError('corrupt metadata accepted')
except Trap:pass
report=dict(passed=True,imports=len(entries),schema=18,checks=['all native imports and constructor slots','real syscall trap and errno failure','channel create TX/RX and guarded close','slot properties and advance','32-bit buflet length and overflow refusal','packet finalize and flow UUID','schema 15/17/19 refusal','redzone corruption refusal'])
a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))

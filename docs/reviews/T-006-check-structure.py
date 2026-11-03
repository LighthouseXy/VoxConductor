# Read-only structure and format check for the recorded T-006 text edits.
from pathlib import Path
import subprocess,re,json,ast,tokenize,io
o=Path(__file__).resolve().parent; r=o.parents[1]
edits=json.loads((o/'T-006-text-edits.json').read_text())
lex=re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',re.S)
def strip_comments(s):return lex.sub(lambda m:'' if m.group().startswith(('//','/*')) else m.group(),s)
def kconfig(s):
 result=[];help_indent=None
 for l in s.splitlines():
  if l.strip().startswith('#'):continue
  if l.strip()=='help':help_indent=len(l)-len(l.lstrip());continue
  if help_indent is not None:
   if not l.strip() or len(l)-len(l.lstrip())>help_indent:continue
   help_indent=None
  if l.strip():result.append(l)
 return result
changed=sorted({e['file'] for e in edits});checked=[]
for f in filter(None,changed):
 before=subprocess.check_output(['git','show','ced2f5ba9898b75a9c31f845738920e2eeb8b6fc:'+f],cwd=r,text=True);after=(r/f).read_text();normalized=after
 for e in reversed(edits):
  if e['file']==f and e['kind']!='comment':
   assert normalized.count(e['after'])==1,(f,e['after']);normalized=normalized.replace(e['after'],e['before'],1)
 if f.endswith(('.c','.h','.mjs')):assert strip_comments(before)==strip_comments(normalized),f
 elif f.endswith('.py'):assert ast.dump(ast.parse(before),include_attributes=False)==ast.dump(ast.parse(normalized),include_attributes=False),f
 elif f.endswith('.html'):assert strip_comments(before)==strip_comments(normalized),f
 elif f.endswith('.projbuild'):assert kconfig(before)==kconfig(normalized),f
 elif f.endswith('.md'):assert before==normalized,f
 elif f=='.gitignore':assert [x for x in before.splitlines() if not x.startswith('#')]==[x for x in normalized.splitlines() if not x.startswith('#')],f
 else:raise AssertionError('unexpected changed file '+f)
 checked.append({'file':f,'executable_structure_unchanged_after_approved_text_normalization':True})
# C format placeholders and the bounded interruption marker remain compatible.
for e in edits:
 if e['kind']=='display_or_explanation' and e['file'].endswith(('.c','.h')):
  formats=lambda s:re.findall(r'%(?:[-+ #0]*\d*(?:\.\*|\.\d+)?[hlzjtL]*[diouxXfFeEgGaAcspn%])',s)
  assert formats(e['before'])==formats(e['after'])
for e in edits:
 if e['file']=='firmware/main/app_cloud.c' and e['kind']!='comment':assert len(e['after'].encode())<=len(e['before'].encode())<=160
print('Structure invariants passed:',len(checked),'files; C formats and marker budget preserved.')

import csv,collections
r=list(csv.DictReader(open('results/core/runs.csv')))
print('solution_valid', collections.Counter(x['solution_valid'] for x in r if x['status']=='completed'))
cellok=collections.defaultdict(bool)
for x in r:
    if x['status']=='completed': cellok[(x['population'],x['n'],x['empty'],x['op'])]=True
cand=[]
for x in r:
    if x['concurrency']!='6': continue
    key=(x['population'],x['n'],x['empty'],x['op'])
    if x['status']=='timeout' and cellok[key]: cand.append((x,'timeout_in_completing_cell'))
    elif x['status']=='completed' and float(x['total_ns'])/1e9>144: cand.append((x,'completed_over_T_over_s'))
print(len(cand), collections.Counter(c[1] for c in cand))
for x,why in cand: print(x['fixture_id'],x['solver'],x['op'],x['status'],round(float(x['total_ns'])/1e9,1),why)
# priority: same solver completed on another seed of the same cell
solok=collections.defaultdict(list)
for x in r:
    if x['status']=='completed': solok[(x['population'],x['n'],x['empty'],x['op'],x['solver'])].append(float(x['total_ns'])/1e9)
p1=[(x,solok[(x['population'],x['n'],x['empty'],x['op'],x['solver'])]) for x,_ in cand if solok[(x['population'],x['n'],x['empty'],x['op'],x['solver'])]]
print('same-solver-other-seed completed:',len(p1))
for x,t in p1: print(x['fixture_id'],x['solver'],x['op'],[round(v,1) for v in t])
# max completed parallel time overall
pc=[float(x['total_ns'])/1e9 for x in r if x['concurrency']=='6' and x['status']=='completed']
print('max parallel completed', max(pc), sorted(pc)[-5:])
import random
p1ids={(x['fixture_id'],x['solver'],x['op']) for x,_ in p1}
rest=[x for x,_ in cand if (x['fixture_id'],x['solver'],x['op']) not in p1ids]
rest.sort(key=lambda x:x['job_id']); random.Random(7).shuffle(rest)
sel=[(x,'borderline: same solver completed on another seed') for x,_ in p1]+[(x,'borderline: another solver completed in cell (sampled)') for x in rest[:40-len(p1)]]
with open('reports/serial_confirm_jobs.csv','w',newline='') as f:
    w=csv.writer(f); w.writerow(['fixture_id','solver','op','budget_s','queue','reason'])
    for x,why in sel: w.writerow([x['fixture_id'],x['solver'],x['op'],180,'serial',why])
print('wrote',len(sel))

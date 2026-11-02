// Z-A-specific SPIR-V transforms, matching android/tools/shader_bridge.py.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;

static class AndroidZaFixes
{
    static List<int[]> Instructions(byte[] bytes,out int[] header)
    {
        if(bytes.Length<20||bytes.Length%4!=0)throw new Exception("Invalid SPIR-V");
        int[] w=new int[bytes.Length/4];Buffer.BlockCopy(bytes,0,w,0,bytes.Length);header=w.Take(5).ToArray();var result=new List<int[]>();
        for(int i=5;i<w.Length;){int n=(int)((uint)w[i]>>16);if(n==0||i+n>w.Length)throw new Exception("Invalid SPIR-V instruction");result.Add(w.Skip(i).Take(n).ToArray());i+=n;}return result;
    }
    static int[] I(int op,params int[] a)=>new[]{(a.Length+1)<<16|op}.Concat(a).ToArray();
    static byte[] Bytes(int[] header,IEnumerable<int[]> instructions,int bound){header[3]=bound;int[] w=header.Concat(instructions.SelectMany(x=>x)).ToArray();byte[] b=new byte[w.Length*4];Buffer.BlockCopy(w,0,b,0,b.Length);return b;}
    internal static byte[] Warp32(byte[] data)
    {
        var inst=Instructions(data,out var h);int bound=h[3],lane=-1,uintType=-1,uintIn=-1;var masks=new HashSet<int>();var ballots=new HashSet<int>();bool hasOps=false;
        int Id()=>bound++;
        foreach(var a in inst){int op=a[0]&65535;if(op==71&&a[2]==11){if(a[3]==41)lane=a[1];else if(a[3]>=4416&&a[3]<=4420)masks.Add(a[1]);}else if(op==21&&a[2]==32&&a[3]==0)uintType=a[1];else if(op==339){ballots.Add(a[2]);hasOps=true;}else if(op is 345 or 334 or 335 or 336)hasOps=true;}
        if(!hasOps&&lane<0)return data;if(uintType<0)throw new Exception("Subgroup operations lack uint32 type");
        foreach(var a in inst)if((a[0]&65535)==32&&a[2]==1&&a[3]==uintType)uintIn=a[1];
        int c5=Id(),c31=Id(),cn31=Id();var globals=new List<int[]>{I(43,uintType,c5,5),I(43,uintType,c31,31),I(43,uintType,cn31,unchecked((int)0xffffffe0))};var deco=new List<int[]>();
        if(lane<0){if(uintIn<0){uintIn=Id();globals.Insert(0,I(32,uintIn,1,uintType));}lane=Id();globals.Add(I(59,uintIn,lane,1));deco.Add(I(71,lane,11,41));}
        var output=new List<int[]>();int first=-1,lastDeco=-1;
        foreach(var original in inst){var a=original;int op=a[0]&65535;if(op==71)lastDeco=output.Count;if(op==54&&first<0){first=output.Count;output.AddRange(globals);}
            if(op==15&&deco.Count>0&&!a.Skip(3).Contains(lane)){a=a.Append(lane).ToArray();a[0]=a.Length<<16|15;}
            if(first<0){output.Add(a);continue;}
            int Shift(){int raw=Id(),comp=Id();output.Add(I(61,uintType,raw,lane));output.Add(I(194,uintType,comp,raw,c5));return comp;}
            if(op==61&&masks.Contains(a[3])){ballots.Add(a[2]);output.Add(a);}
            else if(op==61&&a[3]==lane&&a[1]==uintType){int raw=Id();output.Add(I(61,uintType,raw,lane));output.Add(I(199,uintType,a[2],raw,c31));}
            else if(op==65&&masks.Contains(a[3])&&a.Length==5){int c=Shift();output.Add(I(65,a[1],a[2],a[3],c));}
            else if(op==81&&ballots.Contains(a[3])&&a.Length==5&&a[4]==0){int c=Shift();output.Add(I(77,a[1],a[2],a[3],c));}
            else if(op==345){int raw=Id(),hi=Id(),lo=Id(),nid=Id();output.Add(I(61,uintType,raw,lane));output.Add(I(199,uintType,hi,raw,cn31));output.Add(I(199,uintType,lo,a[5],c31));output.Add(I(197,uintType,nid,hi,lo));output.Add(I(345,a[1],a[2],a[3],a[4],nid));}
            else output.Add(a);
        }
        if(deco.Count>0)output.InsertRange(lastDeco+1,deco);return Bytes(h,output,bound);
    }
    internal static byte[] Xfb(byte[] data,JsonArray xfb)
    {
        if(xfb==null||xfb.Count==0)return data;
        var inst=Instructions(data,out var h);int bound=h[3],entry=-1;int Id()=>bound++;
        var locs=new Dictionary<int,int>();var vars=new Dictionary<int,(int k,int ptr)>();var pointers=new Dictionary<int,int>();var vectors=new Dictionary<(int scalar,int count),int>();
        for(int k=0;k<inst.Count;k++){var a=inst[k];int op=a[0]&65535;if(op==15&&a[1]==0)entry=a[2];else if(op==71&&a.Length>=4&&a[2]==30)locs[a[3]]=a[1];else if(op==59&&a.Length>=4&&a[3]==3)vars[a[2]]=(k,a[1]);else if(op==32&&a.Length>=4&&a[2]==3)pointers[a[3]]=a[1];else if(op==23&&a.Length>=4)vectors[(a[2],a[3])]=a[1];}
        var byVar=new Dictionary<int,(int loc,int comp,int buf,int stride,int off)>();
        foreach(var n in xfb){int loc=n["loc"].GetValue<int>(),comp=n["comp"].GetValue<int>(),buf=n["buf"].GetValue<int>(),stride=n["stride"].GetValue<int>(),off=n["off"].GetValue<int>();
            if(buf<0||buf>=4||comp<1||comp>4||stride<=0||off<0||(long)off+4*comp>stride)throw new Exception("Invalid transform-feedback output");
            if(!locs.TryGetValue(loc,out int vid)||!vars.ContainsKey(vid)||byVar.ContainsKey(vid))throw new Exception("Missing/duplicate transform-feedback location");byVar[vid]=(loc,comp,buf,stride,off);
        }if(entry<0)throw new Exception("Missing XFB entry point");
        var invPtr=pointers.ToDictionary(x=>x.Value,x=>x.Key);var invVec=vectors.ToDictionary(x=>x.Value,x=>x.Key);var globals=new List<int[]>();var changed=new Dictionary<int,(int elem,int comp,int scalar,int count)>();var valueTypes=new Dictionary<int,int>();
        var typed=new HashSet<int>(new[]{1,12,41,42,43,44,45,46,47,48,49,50,51,52,55,57,59,61,65,66,67,68,69,70,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,245});
        foreach(var a in inst){int op=a[0]&65535;if(a.Length>=3&&(typed.Contains(op)||op>=109&&op<=242))valueTypes[a[2]]=a[1];}
        foreach(var item in byVar){int vid=item.Key;var desc=item.Value;var variable=vars[vid];int elem=invPtr.GetValueOrDefault(variable.ptr,-1);
            if(!invVec.TryGetValue(elem,out var v))continue;if(desc.comp>v.count)throw new Exception("XFB component count exceeds output");if(desc.comp==v.count)continue;
            int want;if(desc.comp==1)want=v.scalar;else if(!vectors.TryGetValue((v.scalar,desc.comp),out want)){want=Id();vectors[(v.scalar,desc.comp)]=want;globals.Add(I(23,want,v.scalar,desc.comp));}
            if(!pointers.TryGetValue(want,out int ptr)){ptr=Id();pointers[want]=ptr;globals.Add(I(32,ptr,3,want));}
            inst[variable.k][1]=ptr;changed[vid]=(want,desc.comp,v.scalar,v.count);
        }
        var aliases=new Dictionary<int,int>();var output=new List<int[]>();int capsEnd=0,exec=-1,decoEnd=-1,firstFn=-1;
        foreach(var original in inst){var a=original;int op=a[0]&65535;
            if(op==65&&a.Length>=5&&changed.TryGetValue(a[3],out var chain)&&chain.comp==1){aliases[a[2]]=a[3];continue;}
            if(op==62&&a.Length>=3&&changed.TryGetValue(a[1],out var c)){
                int source=a[2],sourceType=valueTypes.GetValueOrDefault(source,-1);
                if(sourceType!=c.elem){if(!invVec.TryGetValue(sourceType,out var from)||from.scalar!=c.scalar||from.count<c.comp)throw new Exception("Cannot preserve XFB output value");int converted=Id();if(c.comp==1)output.Add(I(81,c.scalar,converted,source,0));else output.Add(I(79,new[]{c.elem,converted,source,source}.Concat(Enumerable.Range(0,c.comp)).ToArray()));source=converted;}
                a=(int[])a.Clone();a[2]=source;
            }else if(aliases.Count>0&&op is not (5 or 6 or 7 or 8 or 71 or 72)){a=(int[])a.Clone();for(int j=1;j<a.Length;j++)a[j]=aliases.GetValueOrDefault(a[j],a[j]);}
            output.Add(a);if(op==17)capsEnd=output.Count;else if(op is 15 or 16)exec=output.Count;else if(op is 71 or 72)decoEnd=output.Count;else if(op==54&&firstFn<0)firstFn=output.Count-1;
        }
        if(globals.Count>0&&firstFn>=0)output.InsertRange(firstFn,globals);
        if(decoEnd>=0){var decos=new List<int[]>();foreach(var item in byVar){decos.Add(I(71,item.Key,36,item.Value.buf));decos.Add(I(71,item.Key,37,item.Value.stride));decos.Add(I(71,item.Key,35,item.Value.off));}output.InsertRange(decoEnd,decos);}
        if(exec>=0&&!output.Any(a=>(a[0]&65535)==16&&a[1]==entry&&a[2]==11))output.Insert(exec,I(16,entry,11));
        if(!output.Any(a=>(a[0]&65535)==17&&a[1]==53))output.Insert(capsEnd,I(17,53));return Bytes(h,output,bound);
    }
}

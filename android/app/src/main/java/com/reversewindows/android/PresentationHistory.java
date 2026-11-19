package com.reversewindows.android;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;

/** One-second monotonic samples from a single visible console session. */
final class PresentationHistory {
    static final int PERIOD=60;
    static final class Sample {
        final double fps,p50,p95,slow;final long at;final int intervals,target;
        Sample(long at,double fps,int target,ArrayList<Double> gaps){
            this.at=at;this.fps=fps;this.target=target;this.intervals=gaps.size();
            double[] values=new double[gaps.size()];int count=0;
            for(int i=0;i<values.length;i++){values[i]=gaps.get(i);if(values[i]>1500.0/target)count++;}
            Arrays.sort(values);p50=percentile(values,.5);p95=percentile(values,.95);
            slow=values.length==0?Double.NaN:100.0*count/values.length;
        }
        private static double percentile(double[] v,double p){return v.length==0?Double.NaN:v[(int)Math.ceil(v.length*p)-1];}
    }
    static final class Episode {
        final long start;long end;double worst;boolean interrupted;
        Episode(long start,double worst){this.start=start;this.worst=worst;}
    }
    final ArrayDeque<Sample> samples=new ArrayDeque<>();
    final ArrayDeque<Episode> episodes=new ArrayDeque<>();
    private final ArrayList<Double> gaps=new ArrayList<>();
    private long lastFrame,begin,expectThrough,lastInput;private int frames,lows,highs,target;
    private double firstWorst;
    private long firstLow,firstHigh;
    private Episode active;
    void clear(){samples.clear();episodes.clear();reset(0);}
    void reset(long wall){
        if(active!=null){active.end=wall;active.interrupted=true;active=null;}
        lows=highs=frames=0;lastFrame=begin=expectThrough=lastInput=0;gaps.clear();
    }
    void input(long ns){lastInput=ns;expectThrough=Math.max(expectThrough,ns+3_500_000_000L);}
    void presented(long ns){
        if(begin==0)return;
        if(lastFrame!=0 && ns>lastFrame && ns-lastFrame<2_000_000_000L){
            gaps.add((ns-lastFrame)/1e6);
            if(ns-lastFrame<250_000_000L)expectThrough=Math.max(expectThrough,ns+1_200_000_000L);
        }
        lastFrame=ns;frames++;
    }
    void tick(long ns,long wall,int effective,boolean visibleAndRunning,boolean sourceAlive){
        if(!visibleAndRunning){if(begin!=0)reset(wall);return;}
        if(begin==0||effective!=target){reset(wall);begin=ns;target=effective;return;}
        long delta=ns-begin;
        if(delta<950_000_000L)return;
        boolean covered=sourceAlive&&delta<1_750_000_000L;
        boolean expected=expectThrough>=begin;
        double rate=covered&&(frames>0||expected)?frames*1e9/delta:Double.NaN;
        Sample sample=new Sample(wall,rate,target,gaps);
        if(samples.size()==PERIOD)samples.removeFirst();
        samples.addLast(sample);
        if(covered&&expected&&!Double.isNaN(rate)){
            if(rate<target*.8){if(lows++==0){firstLow=wall-delta/1_000_000L;firstWorst=rate;}else firstWorst=Math.min(firstWorst,rate);}
            else lows=0;
            if(active==null&&lows>=3){
                active=new Episode(firstLow,firstWorst);if(episodes.size()==30)episodes.removeFirst();episodes.addLast(active);
            }
            if(active!=null){
                active.worst=Math.min(active.worst,rate);
                if(rate>=target*.9){if(highs++==0)firstHigh=wall-delta/1_000_000L;}else highs=0;
                if(highs>=3){active.end=firstHigh;active=null;highs=lows=0;}
            }
        }else{
            lows=highs=0;if(active!=null){active.end=wall;active.interrupted=true;active=null;}
        }
        begin=ns;frames=0;gaps.clear();
        if(!covered||!expected)lastFrame=0;
    }
}

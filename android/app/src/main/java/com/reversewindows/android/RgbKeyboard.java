package com.reversewindows.android;

import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

/** Physical Spanish ISO layout. Each key keeps its PC scan code in every view. */
final class RgbKeyboard extends ViewGroup {
    enum Mode { Completo, ABC, Núm, Fn }
    interface Output { void key(int ascii, int scan, boolean down); }
    private static final class Key {
        final String name, label;
        final int scan, plain, shifted;
        Key(String name, String label, int scan, int plain, int shifted) {
            this.name=name; this.label=label; this.scan=scan; this.plain=plain; this.shifted=shifted;
        }
        boolean modifier() {
            return scan == 0x2a || scan == 0x36 || scan == 0x1d || scan == 0xe01d ||
                   scan == 0x38 || scan == 0xe038 || scan == 0xe05b || scan == 0xe05c;
        }
    }
    private static final class Slot {
        final float x,y,w,h;
        Slot(float x,float y,float w,float h) { this.x=x; this.y=y; this.w=w; this.h=h; }
    }
    private static final Map<String,Key> keys = new HashMap<>();
    private static void key(String name,String label,int scan,int plain,int shift) {
        keys.put(name,new Key(name,label,scan,plain,shift));
    }
    private static void controls(String names,int... codes) {
        String[] parts=names.split(" ");
        for (int i=0;i<codes.length;i++) key(parts[i],parts[i],codes[i],0,0);
    }
    private static void chars(String plain,String shifted,int... scans) {
        for (int i=0;i<scans.length;i++) {
            char c=plain.charAt(i), alt=shifted.charAt(i);
            String name=Character.toString(Character.toUpperCase(c));
            key(name,Character.isLetter(c)?name:(c==alt?name:c+" "+alt),scans[i],c,alt);
        }
    }
    static {
        key("Esc","Esc",1,27,27); key("Tab","Tab",15,9,9);
        key("Enter","Intro",0x1c,13,13); key("Bksp","⌫",14,8,8);
        key("Space","⎵",0x39,32,32);
        for (int i=1;i<=12;i++) key("F"+i,"F"+i,i<=10?0x3a+i:0x57+i-11,0,0);
        chars("1234567890","!\"·$%&/()=",2,3,4,5,6,7,8,9,10,11);
        chars("qwertyuiop","QWERTYUIOP",16,17,18,19,20,21,22,23,24,25);
        chars("asdfghjklñ","ASDFGHJKLÑ",30,31,32,33,34,35,36,37,38,39);
        chars("zxcvbnm","ZXCVBNM",44,45,46,47,48,49,50);
        chars("º'¡`+´ç<,.-","ª?¿^*¨Ç>;:_",41,12,13,26,27,40,43,86,51,52,53);
        controls("Insert Inicio RePág Supr Fin AvPág ← ↑ ↓ →",0xe052,0xe047,0xe049,0xe053,0xe04f,0xe051,0xe04b,0xe048,0xe050,0xe04d);
        key("Prt","Impr\nPant",0xe037,0,0); key("Scr","Bloq\nDespl",0x46,0,0);
        key("Pausa","Pausa",0xe045,0,0); key("Num","Bloq\nNum",0x45,0,0);
        key("Caps","Bloq Mayús",0x3a,0,0);
        key("Shift","⇧",0x2a,0,0); key("RShift","⇧ Mayús",0x36,0,0);
        key("Ctrl","Ctrl",0x1d,0,0); key("RCtrl","Ctrl",0xe01d,0,0);
        key("Alt","Alt",0x38,0,0); key("AltGr","Alt Gr",0xe038,0,0);
        key("Win","⊞",0xe05b,0,0); key("RWin","⊞",0xe05c,0,0); key("Menu","☰",0xe05d,0,0);
        int[] pad={82,79,80,81,75,76,77,71,72,73};
        for (int i=0;i<10;i++) key("N"+i,Integer.toString(i),pad[i],'0'+i,'0'+i);
        key("N/","/",0xe035,'/','/');key("N*","*",55,'*','*');key("N-","−",74,'-','-');
        key("N+","+",78,'+','+');key("N.","·",83,'.','.');key("NEnter","Intro",0xe01c,13,13);
        if (keys.size()!=105) throw new IllegalStateException("Teclado ISO incompleto: "+keys.size());
    }
    private final MainActivity activity;
    private final Output output;
    private final Set<Integer> held=new HashSet<>(), latched=new HashSet<>(), usedModifiers=new HashSet<>();
    private final Map<Integer,Integer> activeChars=new HashMap<>();
    private final ArrayList<Slot> slots=new ArrayList<>();
    private boolean caps;
    private Mode mode;
    private int intensity;
    RgbKeyboard(MainActivity activity,Output output,Mode mode,int intensity) {
        super(activity);
        this.activity=activity; this.output=output; this.mode=mode; this.intensity=intensity;
        setBackground(activity.panel(0xff15191e,6,0xff40474f));
        setMotionEventSplittingEnabled(true);
        build();
    }
    Mode mode() {return mode;}
    void setMode(Mode next) {release(); mode=next; build();}
    void light(int level) {intensity=level; for(int i=0;i<getChildCount();i++) style((Button)getChildAt(i),slots.get(i));}
    void release() {
        for(Map.Entry<Integer,Integer> k:new HashMap<>(activeChars).entrySet()) output.key(k.getValue(),k.getKey(),false);
        for(int scan:held) output.key(0,scan,false);
        for(int scan:latched) if(!held.contains(scan)) output.key(0,scan,false);
        held.clear();latched.clear();usedModifiers.clear();activeChars.clear();highlight();
    }
    private void highlight() {
        for(int i=0;i<getChildCount();i++){
            View b=getChildAt(i);Key k=keys.get((String)b.getTag());
            b.setSelected(k!=null && (latched.contains(k.scan)||held.contains(k.scan)||(caps&&k.scan==0x3a)));
        }
    }
    private static int biosChar(int ch) {
        if(ch<128)return ch;
        switch(ch) {
            case 'ñ':return 164; case 'Ñ':return 165; case 'ç':return 135; case 'Ç':return 128;
            case 'º':return 167; case 'ª':return 166; case '¡':return 173; case '¿':return 168;
            case '·':return 250; default:return 0;
        }
    }
    private void down(Key k) {
        if(k.modifier()){
            if(held.add(k.scan)&&!latched.contains(k.scan))output.key(0,k.scan,true);
            highlight();return;
        }
        usedModifiers.addAll(held);
        if(k.scan==0x3a)caps=!caps;
        boolean shifted=held.contains(42)||latched.contains(42)||held.contains(54)||latched.contains(54);
        if(Character.isLetter(k.plain))shifted^=caps;
        int ascii=biosChar(shifted?k.shifted:k.plain);
        if((held.contains(29)||latched.contains(29)) && k.plain>='a' && k.plain<='z')ascii=k.plain-'a'+1;
        activeChars.put(k.scan,ascii);output.key(ascii,k.scan,true);highlight();
    }
    private void up(Key k,boolean cancelled) {
        if(k.modifier()){
            if(!held.remove(k.scan))return;
            if(cancelled||usedModifiers.remove(k.scan)||latched.remove(k.scan)){
                latched.remove(k.scan);output.key(0,k.scan,false);
            }else latched.add(k.scan);
        }else{
            Integer ascii=activeChars.remove(k.scan);
            if(ascii!=null)output.key(ascii,k.scan,false);
            for(int scan:new HashSet<>(latched))if(!held.contains(scan)){output.key(0,scan,false);latched.remove(scan);}
        }
        highlight();
    }
    private void chord(String name){
        boolean ctrl=held.contains(29)||latched.contains(29);
        if(!ctrl)output.key(0,29,true);
        Key k=keys.get(name);
        output.key(name.equals("C")?3:22,k.scan,true);output.key(name.equals("C")?3:22,k.scan,false);
        if(!ctrl)output.key(0,29,false);
    }
    private void build(){
        removeAllViews();slots.clear();
        if(mode==Mode.Completo) desktop();else thumb();
        requestLayout();
    }
    private void desktop(){
        place("Esc",0,0,1,1);
        for(int i=0;i<12;i++)place("F"+(i+1),1.55f+i+(i/4)*.43f,0,1,1);
        row(15.4f,0,3,"Prt Scr Pausa"); row(18.65f,0,4,"Copiar Pegar ABC");
        strip(0,1,"º 1 2 3 4 5 6 7 8 9 0 ' ¡ Bksp",1,1,1,1,1,1,1,1,1,1,1,1,1,2);
        strip(0,2,"Tab Q W E R T Y U I O P ` +",1.5f,1,1,1,1,1,1,1,1,1,1,1,1);
        place("Enter",13.65f,2,1.35f,2);
        strip(0,3,"Caps A S D F G H J K L Ñ ´ Ç",1.65f,1,1,1,1,1,1,1,1,1,1,1,1);
        strip(0,4,"Shift < Z X C V B N M , . - RShift",1.3f,1,1,1,1,1,1,1,1,1,1,1,2.7f);
        strip(0,5,"Ctrl Win Alt Space AltGr RWin Menu RCtrl",1.25f,1.25f,1.25f,6.25f,1.25f,1.25f,1.25f,1.25f);
        row(15.4f,1,3,"Insert Inicio RePág");row(15.4f,2,3,"Supr Fin AvPág");
        place("↑",16.4f,4,1,1);row(15.4f,5,3,"← ↓ →");
        row(18.65f,1,4,"Num N/ N* N-");row(18.65f,2,3,"N7 N8 N9");
        row(18.65f,3,3,"N4 N5 N6");row(18.65f,4,3,"N1 N2 N3");
        place("N+",21.65f,2,1,2);place("NEnter",21.65f,4,1,2);place("N0",18.65f,5,2,1);place("N.",20.65f,5,1,1);
    }
    private void thumb(){
        String[] rows=switch(mode){
            case ABC -> new String[]{"Q W E R T Y U I O P","A S D F G H J K L Ñ","Shift Z X C V B N M Bksp","Ctrl Alt , Space . Enter"};
            case Núm -> new String[]{"1 2 3 4 5 6 7 8 9 0","N7 N8 N9 N/ N* N-","N4 N5 N6 N1 N2 N3 N+","N0 N. Bksp Space Enter"};
            default -> new String[]{"Esc F1 F2 F3 F4 F5 F6","Tab F7 F8 F9 F10 F11 F12","Ctrl Alt AltGr Win Inicio Fin Supr","Shift Insert RePág AvPág Space Enter"};
        };
        for(int i=0;i<4;i++) {
            if(mode==Mode.ABC&&i==3)strip(0,i,rows[i],1.05f,1.05f,.8f,4.5f,.8f,1.8f);
            else row(0,i,10,rows[i]);
        }
        row(10.15f,0,3.5f,"ABC Núm");row(10.15f,1,3.5f,"Fn Completo");
        row(10.15f,2,3.5f,"Copiar ↑ Pegar");row(10.15f,3,3.5f,"← ↓ →");
    }
    private void row(float x,int y,float width,String names){
        String[] list=names.split(" ");float w=width/list.length;
        for(String name:list){place(name,x,y,w,1);x+=w;}
    }
    private void strip(float x,int y,String names,float... widths){
        String[] list=names.split(" ");
        for(int i=0;i<list.length;i++){place(list[i],x,y,widths[i],1);x+=widths[i];}
    }
    private void place(String name,float x,float y,float w,float h){
        Key k=keys.get(name);Slot slot=new Slot(x,y,w,h);slots.add(slot);
        Button b=new Button(activity);b.setTag(name);b.setText(k==null?name:k.label);
        b.setContentDescription(k!=null && name.matches("N[0-9+*/.\\-]")?"Numérico "+k.label:name.equals("Space")?"Espacio":k==null?name:k.label.replace('\n',' '));
        b.setAllCaps(false);b.setMinWidth(0);b.setMinimumWidth(0);b.setMinHeight(0);b.setMinimumHeight(0);
        b.setPadding(0,0,0,0);b.setGravity(Gravity.CENTER);b.setMaxLines(2);
        b.setTextColor(0xfff2f5fa);b.setTypeface(Typeface.create("sans-serif-medium",Typeface.BOLD));
        b.setStateListAnimator(null);b.setTextSize(mode==Mode.Completo?10:16);
        b.setAutoSizeTextTypeUniformWithConfiguration(7,mode==Mode.Completo?13:20,1,TypedValue.COMPLEX_UNIT_SP);
        style(b,slot);
        if(k==null){
            b.setOnClickListener(v->{if(name.equals("Copiar")||name.equals("Pegar"))chord(name.equals("Copiar")?"C":"V");else setMode(Mode.valueOf(name));});
        }else{
            b.setOnClickListener(v->{down(k);up(k,false);});
            b.setOnTouchListener((v,e)->{
                switch(e.getActionMasked()){
                    case MotionEvent.ACTION_DOWN -> {b.setPressed(true);down(k);return true;}
                    case MotionEvent.ACTION_UP,MotionEvent.ACTION_CANCEL -> {
                        b.setPressed(false);up(k,e.getActionMasked()==MotionEvent.ACTION_CANCEL);return true;
                    }
                    default -> {return true;}
                }
            });
        }
        addView(b);
    }
    private float cols(){return mode==Mode.Completo?22.65f:13.65f;}
    private void style(Button b,Slot s){
        float t=(s.x+s.w*.5f)/cols();
        int hue=Color.HSVToColor(new float[]{(325f-t*316f+360f)%360f,.76f,.95f});
        int edge=android.graphics.Color.rgb((70*(100-intensity)+Color.red(hue)*intensity)/100,
                (77*(100-intensity)+Color.green(hue)*intensity)/100,(86*(100-intensity)+Color.blue(hue)*intensity)/100);
        android.graphics.drawable.StateListDrawable states=new android.graphics.drawable.StateListDrawable();
        for(boolean selected:new boolean[]{true,false}){
            GradientDrawable cap=new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                    selected?new int[]{0xff39645e,0xff28413f}:new int[]{0xff373e48,0xff171b21});
            cap.setCornerRadius(activity.dp(4));cap.setStroke(Math.max(1,activity.dp(1)),edge);
            states.addState(selected?new int[]{android.R.attr.state_selected}:new int[]{},cap);
        }
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(0x50a6ffee),states,null));
    }
    @Override protected void onMeasure(int ws,int hs){
        int w=MeasureSpec.getSize(ws),h=MeasureSpec.getSize(hs);setMeasuredDimension(w,h);
        for(int i=0;i<getChildCount();i++){
            android.graphics.Rect r=bounds(slots.get(i),w,h);
            getChildAt(i).measure(MeasureSpec.makeMeasureSpec(r.width(),MeasureSpec.EXACTLY),MeasureSpec.makeMeasureSpec(r.height(),MeasureSpec.EXACTLY));
        }
    }
    private android.graphics.Rect bounds(Slot s,int width,int height){
        int pad=activity.dp(3),gap=activity.dp(1);
        float u=(width-2f*pad)/cols(),v=(height-2f*pad)/(mode==Mode.Completo?6:4);
        int l=pad+Math.round(s.x*u)+gap,t=pad+Math.round(s.y*v)+gap;
        return new android.graphics.Rect(l,t,Math.max(l+1,pad+Math.round((s.x+s.w)*u)-gap),
                Math.max(t+1,pad+Math.round((s.y+s.h)*v)-gap));
    }
    @Override protected void onLayout(boolean changed,int l,int t,int r,int b){
        for(int i=0;i<getChildCount();i++){android.graphics.Rect s=bounds(slots.get(i),r-l,b-t);getChildAt(i).layout(s.left,s.top,s.right,s.bottom);}
    }
}

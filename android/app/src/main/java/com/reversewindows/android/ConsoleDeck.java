package com.reversewindows.android;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextView;

/** The monitor and its inputs remain attached while a session runs. */
final class ConsoleDeck extends LinearLayout {
    interface Input extends RgbKeyboard.Output { void pointer(int dx,int dy,int buttons); }
    final RgbKeyboard keyboard;
    private final MainActivity activity;
    private final Input input;
    private final FrameLayout idle;
    private final TextView caption;
    private final Pad pad;
    private final FrameLayout screen;
    private final Button[] clicks=new Button[2];
    private int mask;
    private float sensitivity;
    ConsoleDeck(MainActivity a,FrameLayout screen,Input input,RgbKeyboard.Mode mode,int light,float sensitivity,
                Runnable settings,Runnable metrics) {
        super(a);this.activity=a;this.screen=screen;this.input=input;this.sensitivity=sensitivity;
        setOrientation(VERTICAL);setPadding(a.dp(6),a.dp(4),a.dp(6),a.dp(4));setBackgroundColor(0xff171c22);
        LinearLayout top=new LinearLayout(a);top.setMotionEventSplittingEnabled(true);
        addView(top,new LayoutParams(-1,0,.41f));
        FrameLayout display=new FrameLayout(a);display.setClipToOutline(true);
        display.setBackground(a.panel(0xff0c1218,6,0xff46505b));
        top.addView(display,new LayoutParams(0,-1,.61f));
        display.addView(screen,new FrameLayout.LayoutParams(-1,-1));
        idle=new FrameLayout(a);display.addView(idle,new FrameLayout.LayoutParams(-1,-1));
        ImageView art=new ImageView(a);art.setScaleType(ImageView.ScaleType.CENTER_CROP);
        art.setImageResource(R.drawable.console_idle);art.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        idle.addView(art,new FrameLayout.LayoutParams(-1,-1));
        caption=a.label("Máquina apagada",12,0xffe2f6f0);
        caption.setBackground(a.panel(0xbb0c2024,5,0x55398d81));caption.setPadding(a.dp(9),a.dp(4),a.dp(9),a.dp(4));
        FrameLayout.LayoutParams cp=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.LEFT);
        cp.setMargins(a.dp(8),0,0,a.dp(7));idle.addView(caption,cp);
        Button menu=a.action("☰",false,()->{});menu.setId(R.id.vm_console_menu);menu.setTextSize(20);
        menu.setContentDescription("Menú de la consola");menu.setPadding(0,0,0,0);
        menu.setBackground(a.panel(0xe2212a31,6,0xff56616c));
        FrameLayout.LayoutParams mp=new FrameLayout.LayoutParams(a.dp(48),a.dp(48),Gravity.TOP|Gravity.LEFT);
        mp.setMargins(a.dp(4),a.dp(3),0,0);display.addView(menu,mp);
        menu.setOnClickListener(v->{
            release();
            PopupMenu popup=new PopupMenu(a,menu);
            popup.getMenu().add("Configuración").setOnMenuItemClickListener(item->{settings.run();return true;});
            popup.getMenu().add("Métricas").setOnMenuItemClickListener(item->{metrics.run();return true;});
            popup.show();
        });
        LinearLayout pointers=new LinearLayout(a);pointers.setOrientation(VERTICAL);pointers.setMotionEventSplittingEnabled(true);
        LayoutParams pp=new LayoutParams(0,-1,.39f);pp.leftMargin=a.dp(5);top.addView(pointers,pp);
        pad=new Pad(a);pointers.addView(pad,new LayoutParams(-1,0,1));
        LinearLayout row=new LinearLayout(a);row.setMotionEventSplittingEnabled(true);
        LayoutParams rp=new LayoutParams(-1,a.dp(48));rp.topMargin=a.dp(3);pointers.addView(row,rp);
        for(int i=0;i<2;i++){
            int bit=1<<i;String name=i==0?"Clic izquierdo":"Clic derecho";
            Button b=a.action(name,false,()->{button(bit,true);button(bit,false);});
            b.setId(i==0?R.id.vm_left_click:R.id.vm_right_click);b.setTextColor(0xfff1f4fc);
            b.setBackground(a.panel(0xff292f37,4,0xff535e6b));clicks[i]=b;
            b.setOnTouchListener((v,e)->{
                switch(e.getActionMasked()){
                    case MotionEvent.ACTION_DOWN -> {b.setPressed(true);button(bit,true);return true;}
                    case MotionEvent.ACTION_UP,MotionEvent.ACTION_CANCEL -> {b.setPressed(false);button(bit,false);return true;}
                    default -> {return true;}
                }
            });
            LayoutParams bp=new LayoutParams(0,-1,1);if(i>0)bp.leftMargin=a.dp(4);row.addView(b,bp);
        }
        keyboard=new RgbKeyboard(a,input,mode,light);
        LayoutParams kp=new LayoutParams(-1,0,.59f);kp.topMargin=a.dp(5);addView(keyboard,kp);
    }
    void sensitivity(float value) {sensitivity=value;}
    void state(boolean live,CharSequence description) {
        idle.setVisibility(live?GONE:VISIBLE);screen.setVisibility(live?VISIBLE:GONE);
        if(!live)caption.setText(description);
    }
    private void button(int bit,boolean pressed){
        pad.flush();
        int next=pressed?mask|bit:mask&~bit;
        if(next!=mask){mask=next;input.pointer(0,0,mask);}
    }
    void release(){
        keyboard.release();pad.flush();if(mask!=0){mask=0;input.pointer(0,0,0);}
        for(Button b:clicks)b.setPressed(false);
    }
    private final class Pad extends View{
        private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        private int finger=-1;private float lastX,lastY,dx,dy;
        private final Runnable sendMotion=this::flush;
        Pad(MainActivity a){super(a);setId(R.id.vm_touchpad);setContentDescription("Panel táctil");
            setBackground(a.panel(0xff20252b,5,0xff49525c));setClickable(true);}
        @Override protected void onDraw(Canvas c){
            super.onDraw(c);float d=getResources().getDisplayMetrics().density;
            paint.setColor(0xff535b63);
            for(float x=5*d;x<getWidth()-4*d;x+=5*d)for(float y=5*d;y<getHeight()-4*d;y+=5*d)
                c.drawCircle(x,y,.43f*d,paint);
            paint.setColor(0xff818d96);float x=getWidth()/2f,y=getHeight()/2f;
            c.drawLine(x-4*d,y,x+4*d,y,paint);c.drawLine(x,y-4*d,x,y+4*d,paint);
        }
        void flush(){
            removeCallbacks(sendMotion);
            int x=(int)dx,y=(int)dy;dx-=x;dy-=y;
            if(x!=0||y!=0)input.pointer(x,-y,mask);
        }
        @Override public boolean onTouchEvent(MotionEvent e){
            int action=e.getActionMasked(),index=e.findPointerIndex(finger);
            if(action==MotionEvent.ACTION_DOWN){finger=e.getPointerId(0);lastX=e.getX();lastY=e.getY();return true;}
            if(index>=0&&(action==MotionEvent.ACTION_MOVE||action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_POINTER_UP)){
                float x=e.getX(index),y=e.getY(index),gain=sensitivity/getResources().getDisplayMetrics().density;
                dx+=(x-lastX)*gain;dy+=(y-lastY)*gain;lastX=x;lastY=y;
                if(action==MotionEvent.ACTION_MOVE){removeCallbacks(sendMotion);postOnAnimation(sendMotion);}
            }
            if(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL||
                    (action==MotionEvent.ACTION_POINTER_UP&&e.getPointerId(e.getActionIndex())==finger)){
                flush();finger=-1;dx=dy=0;
            }
            return true;
        }
        @Override public boolean performClick(){super.performClick();return true;}
    }
}

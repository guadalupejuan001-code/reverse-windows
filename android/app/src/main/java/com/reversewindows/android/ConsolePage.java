package com.reversewindows.android;

import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Locale;

/** Two-pane layout used by the configuration and measurement windows. */
final class ConsolePage extends LinearLayout {
    private final MainActivity a;
    private final LinearLayout tabs, stack;
    private final ScrollView content;
    private final TextView title;
    private final ArrayList<Button> choices=new ArrayList<>();
    private final ArrayList<LinearLayout> sections=new ArrayList<>();
    private Runnable onChange=()->{};
    ConsolePage(MainActivity a,ScrollView scroll,String name,Runnable back,String other,Runnable switchPage) {
        super(a);this.a=a;this.content=scroll;setOrientation(HORIZONTAL);setBackgroundColor(0xff111920);
        ScrollView railScroll=new ScrollView(a);railScroll.setFillViewport(true);railScroll.setBackgroundColor(0xff17212a);
        int width=Math.max(98,Math.min(146,a.getResources().getConfiguration().screenWidthDp/6));
        addView(railScroll,new LayoutParams(a.dp(width),-1));
        LinearLayout rail=new LinearLayout(a);rail.setOrientation(VERTICAL);rail.setPadding(a.dp(6),a.dp(4),a.dp(6),a.dp(4));railScroll.addView(rail);
        Button home=a.action("‹  Consola",false,back);home.setBackgroundColor(0x00000000);
        rail.addView(home,new LayoutParams(-1,a.dp(40)));
        View stripe=new View(a);stripe.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{0xffed64a7,0xff9990f7,0xff5dd6f0,0xff7bedbe,0xfff4af6c}));rail.addView(stripe,new LayoutParams(-1,a.dp(1)));
        TextView nameView=a.label(name.toUpperCase(Locale.ROOT),10,0xffa8bac4);nameView.setTypeface(Typeface.DEFAULT_BOLD);
        nameView.setPadding(a.dp(4),a.dp(7),0,a.dp(4));rail.addView(nameView);
        tabs=new LinearLayout(a);tabs.setOrientation(VERTICAL);rail.addView(tabs);
        rail.addView(new View(a),new LayoutParams(1,0,1));
        Button jump=a.action(other,false,switchPage);jump.setBackgroundColor(0x00000000);jump.setTextColor(0xffa5ead7);rail.addView(jump,new LayoutParams(-1,a.dp(40)));
        View divider=new View(a);divider.setBackgroundColor(0xff36444d);addView(divider,new LayoutParams(a.dp(1),-1));
        addView(scroll,new LayoutParams(0,-1,1));scroll.setFillViewport(true);
        stack=new LinearLayout(a);stack.setOrientation(VERTICAL);stack.setPadding(a.dp(12),a.dp(8),a.dp(12),a.dp(12));scroll.addView(stack);
        title=a.label("",23,MainActivity.TEXT);title.setTypeface(Typeface.DEFAULT_BOLD);stack.addView(title);
    }
    LinearLayout tab(String label,String heading){
        final int index=sections.size();String[] names={"◈","▤","⌁","▧","◷"};
        Button button=a.action(names[index%names.length]+"   "+label,false,()->select(index));
        button.setContentDescription(label);button.setGravity(Gravity.CENTER_VERTICAL|Gravity.START);button.setMinHeight(0);
        button.setPadding(a.dp(7),0,a.dp(2),0);tabs.addView(button,new LayoutParams(-1,a.dp(39)));
        choices.add(button);
        LinearLayout panel=new LinearLayout(a);panel.setOrientation(VERTICAL);panel.setTag(heading);sections.add(panel);
        stack.addView(panel,new LayoutParams(-1,-2));
        if(index==0)select(0);else {panel.setVisibility(GONE);button.setBackgroundColor(0x00000000);}
        return panel;
    }
    void changed(Runnable action){onChange=action;}
    void select(int selected){
        for(int i=0;i<sections.size();i++){
            boolean active=i==selected;Button btn=choices.get(i);
            sections.get(i).setVisibility(active?VISIBLE:GONE);btn.setSelected(active);
            if(active){btn.setTextColor(0xff8bf1ce);btn.setBackground(a.panel(0xff213b37,5,0xff345a52));
                title.setText((String)sections.get(i).getTag());}
            else {btn.setTextColor(0xffc1ced7);btn.setBackgroundColor(0x00000000);}
        }
        content.scrollTo(0,0);onChange.run();
    }
}

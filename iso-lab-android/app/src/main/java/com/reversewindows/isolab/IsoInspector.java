package com.reversewindows.isolab;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** Fast header inspection. Does not pretend to parse all Windows installation files. */
public final class IsoInspector {
    private IsoInspector(){}
    public static String quick(InputStream source) throws IOException {
        final int sector=2048;
        byte[] first=new byte[18*sector];
        int n=0;
        while(n<first.length){
            int got=source.read(first,n,first.length-n);
            if(got<0)break;
            if(got==0){int c=source.read();if(c<0)break;first[n++]=(byte)c;}
            else n+=got;
        }
        if(n<17*sector+7)return "Archivo demasiado corto para cabecera ISO9660.";
        int off=16*sector;
        boolean pvd=first[off]==1 &&
            new String(first,off+1,5,StandardCharsets.US_ASCII).equals("CD001");
        if(!pvd)return "Cabecera ISO9660 estándar no identificada. Todavía puede analizarse por bloques.";
        String label=new String(first,off+40,32,StandardCharsets.US_ASCII).trim();
        int boot=17*sector;
        boolean elTorito=n>=18*sector && first[boot]==0 &&
            new String(first,boot+1,5,StandardCharsets.US_ASCII).equals("CD001") &&
            new String(first,boot+7,23,StandardCharsets.US_ASCII)
              .equals("EL TORITO SPECIFICATION");
        return "ISO9660 detectado · Volumen: "+(label.isEmpty()?"(sin etiqueta)":label)+
             "\nEl Torito (indicio de arranque BIOS/UEFI): "+
             (elTorito?"cabecera detectada":"no detectado en el primer descriptor")+
             "\nInspección rápida: no modifica ni extrae archivos.";
    }
}

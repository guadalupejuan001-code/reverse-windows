package com.reversewindows.isolab;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Random;

/** Generates a valid El Torito bootable ISO with original x86 guest code,
 *  then compresses it with the SAME Java codec shipped in Android APK. */
public final class RisoInteropFixture {
    private static final int BLOCK=65536, SECTOR=2048;
    private static void le16(byte[] data,int at,int x) {
        data[at]=(byte)x;data[at+1]=(byte)(x>>8);
    }
    private static void le32(byte[] data,int at,long x) {
        for(int i=0;i<4;i++)data[at+i]=(byte)(x>>(i*8));
    }
    private static void descriptor(byte[] iso,int lba,int kind) {
        int at=lba*SECTOR;
        iso[at]=(byte)kind;
        byte[] tag="CD001".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(tag,0,iso,at+1,tag.length);
        iso[at+6]=1;
    }
    private static byte[] fixture(){
        byte[] iso=new byte[BLOCK*6];
        descriptor(iso,16,1);
        descriptor(iso,17,0);
        descriptor(iso,18,255);
        byte[] tag="EL TORITO SPECIFICATION".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(tag,0,iso,17*SECTOR+7,tag.length);
        le32(iso,17*SECTOR+0x47,20);

        int cat=20*SECTOR;
        iso[cat]=1; iso[cat+1]=0; // x86 BIOS El Torito
        iso[cat+30]=0x55; iso[cat+31]=(byte)0xaa;
        int sum=0;
        for(int i=0;i<32;i+=2)
            sum=(sum+(iso[cat+i]&255)+((iso[cat+i+1]&255)<<8))&65535;
        le16(iso,cat+28,-sum);
        iso[cat+32]=(byte)0x88; // bootable, no emulation
        iso[cat+33]=0;
        le16(iso,cat+34,0); // BIOS default load segment 07c0
        le16(iso,cat+38,1); // one 512-byte load sector
        le32(iso,cat+40,24);
        byte[] code={(byte)0xb8,0x34,0x12,(byte)0xa3,0x00,(byte)0x91,(byte)0xf4};
        System.arraycopy(code,0,iso,24*SECTOR,code.length);

        byte[] random=new byte[BLOCK];
        new Random(987321L).nextBytes(random);
        System.arraycopy(random,0,iso,BLOCK,BLOCK);
        System.arraycopy(random,0,iso,BLOCK*2,BLOCK); // REF
        byte[] motif="REVERSE-WINDOWS-ISO-".getBytes(StandardCharsets.US_ASCII);
        for(int i=0;i<BLOCK;i++)iso[BLOCK*3+i]=motif[i%motif.length]; // PATTERN
        // BLOCK*4 deliberately all zeros (ZERO)
        System.arraycopy(random,0,iso,BLOCK*5,BLOCK); // REF
        return iso;
    }
    public static void main(String[] args)throws Exception {
        if(args.length!=2)throw new IllegalArgumentException("Usage: output.iso output.riso");
        Path iso=Path.of(args[0]),riso=Path.of(args[1]);
        byte[] bytes=fixture();
        Files.write(iso,bytes);
        IsoArchive.Result result;
        try(InputStream in=Files.newInputStream(iso);OutputStream out=Files.newOutputStream(riso)){
            result=IsoArchive.compact(in,out,bytes.length,null,()->false);
        }
        require(result.duplicate>=2 && result.zero>=1 && result.periodic>=1,
                "Fixture must exercise REISO01 REF, ZERO and PATTERN");
        try(var in=java.nio.channels.FileChannel.open(riso,java.nio.file.StandardOpenOption.READ)){
            var validated=IsoArchive.verify(in,null,()->false);
            require(validated.originalBytes==bytes.length &&
                    validated.sha256.equals(result.sha256),
                    "Android Java codec did not validate entire RISO");
        }
        System.out.println("REISO01_JAVA_INTEROP_PASS original="+result.originalBytes+
                " compact="+result.archiveBytes+" duplicate="+result.duplicate+
                " zero="+result.zero+" pattern="+result.periodic);
    }
    private static void require(boolean ok,String message){
        if(!ok)throw new IllegalStateException(message);
    }
}

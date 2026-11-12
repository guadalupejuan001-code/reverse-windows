package com.reversewindows.isolab;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Arrays;
import java.util.Random;

/** Standalone host regression, built with javac and run without Android. */
public final class IsoArchiveTest {
    private static void require(boolean yes,String why){if(!yes)throw new AssertionError(why);}
    private static byte[] fixture(){
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        byte[] random=new byte[IsoArchive.BLOCK];
        new Random(1181L).nextBytes(random);
        byte[] words=new byte[IsoArchive.BLOCK];
        for(int i=0;i<words.length;i++)words[i]=(byte)("WINDOWS-REVERSE-ISO-LAB!".charAt(i%24));
        byte[] zeros=new byte[IsoArchive.BLOCK];
        try {
            for(int i=0;i<30;i++){
                out.write(random);out.write(words);out.write(zeros);
                if(i%3==0)out.write(new byte[]{5,8,13,21,34,55});
            }
            out.write(random,0,107);
        }catch(IOException impossible){throw new IllegalStateException(impossible);}
        return out.toByteArray();
    }
    private static void roundtrip(byte[] source) throws Exception {
        ByteArrayOutputStream archive=new ByteArrayOutputStream();
        IsoArchive.Result packed=IsoArchive.compact(new ByteArrayInputStream(source),archive,
                source.length,(done,total)->{},()->false);
        require(packed.originalBytes==source.length,"Input byte count mismatch");
        require(packed.archiveBytes==archive.size(),"Archive byte count mismatch");
        Path path=Files.createTempFile("reverse-iso-",".riso");
        try{
            Files.write(path,archive.toByteArray());
            ByteArrayOutputStream original=new ByteArrayOutputStream();
            try(FileChannel input=FileChannel.open(path,StandardOpenOption.READ)){
                IsoArchive.Result restored=IsoArchive.restore(input,original,Files.size(path),
                        (done,total)->{},()->false);
                require(restored.originalBytes==source.length,"Output size mismatch");
                require(restored.sha256.equals(packed.sha256),"SHA mismatch");
            }
            require(Arrays.equals(source,original.toByteArray()),
                    "Not byte-identical after full reconstruction");
        }finally{Files.deleteIfExists(path);}
        System.out.println("REVERSE_ISO_LOSSLESS_PASS original="+source.length+
                " archive="+packed.archiveBytes+" zero="+packed.zero+
                " pattern="+packed.periodic+" duplicates="+packed.duplicate+
                " deflated="+packed.deflated+" sha256="+packed.sha256);
    }
    private static void mutationMustFail() throws Exception{
        byte[] original=new byte[IsoArchive.BLOCK];
        new Random(41).nextBytes(original);
        ByteArrayOutputStream archive=new ByteArrayOutputStream();
        IsoArchive.compact(new ByteArrayInputStream(original),archive,
                original.length,null,()->false);
        byte[] bytes=archive.toByteArray();
        bytes[bytes.length-1]^=1; // Corrupt saved full ISO digest in footer.
        Path path=Files.createTempFile("reverse-iso-corrupt-",".riso");
        try{
            Files.write(path,bytes);
            boolean rejected=false;
            try(FileChannel input=FileChannel.open(path,StandardOpenOption.READ)){
                IsoArchive.restore(input,new ByteArrayOutputStream(),bytes.length,null,()->false);
            }catch(IOException expected){rejected=true;}
            require(rejected,"Corrupted archive was silently accepted");
        }finally{Files.deleteIfExists(path);}
        System.out.println("REVERSE_ISO_CORRUPTION_REJECTED");
    }
    public static void main(String[] args) throws Exception {
        roundtrip(new byte[0]);
        roundtrip(new byte[]{0});
        roundtrip(new byte[]{1,2,3});
        roundtrip(fixture());
        byte[] incompressible=new byte[IsoArchive.BLOCK*4+37];
        new Random(9317L).nextBytes(incompressible);
        roundtrip(incompressible);
        mutationMustFail();
        boolean cancellation=false;
        try{
            IsoArchive.compact(new ByteArrayInputStream(fixture()),new ByteArrayOutputStream(),
                    -1,null,()->true);
        }catch(IOException expected){cancellation=true;}
        require(cancellation,"Cancellation was not honored");
        System.out.println("REVERSE_ISO_CANCEL_PASS");
    }
}

package com.reversewindows.isolab;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.DataFormatException;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * REVERSE ISO Lab v0.1: self-contained, lossless chunk archive.
 * No ISO bytes or keys are sent to a server. This is a measured baseline:
 * not a claim that Ether / GPU-VIRTUAL / qSSD currently compress the ISO.
 */
public final class IsoArchive {
    private IsoArchive() {}
    private static final byte[] MAGIC = {'R','E','I','S','O','0','1','\n'};
    public static final int BLOCK = 65536;
    private static final int RAW=1, DEFLATE=2, REF=3, ZERO=4, PATTERN=5, END=127;
    private static final int MAX_PATTERN=64;
    private static final byte[] ZEROS = new byte[BLOCK];

    public interface Progress { void update(long done, long total); }
    public interface Cancel { boolean requested(); }

    public static final class Result {
        public final long originalBytes, archiveBytes, blocks, unique, duplicate, zero, periodic, deflated, raw;
        public final String sha256;
        private Result(long originalBytes, long archiveBytes, long blocks, long unique,
                       long duplicate, long zero, long periodic, long deflated, long raw, String sha256) {
            this.originalBytes=originalBytes; this.archiveBytes=archiveBytes;
            this.blocks=blocks; this.unique=unique; this.duplicate=duplicate;
            this.zero=zero; this.periodic=periodic; this.deflated=deflated; this.raw=raw;
            this.sha256=sha256;
        }
        public double reductionPercent() {
            return originalBytes == 0 ? 0 : (100.0 * (originalBytes - archiveBytes) / originalBytes);
        }
    }

    private static final class Key {
        final byte[] hash;
        final int length, code;
        Key(byte[] hash, int length) {
            this.hash=hash; this.length=length;
            this.code=31*Arrays.hashCode(hash)+length;
        }
        @Override public int hashCode() { return code; }
        @Override public boolean equals(Object other) {
            if (!(other instanceof Key)) return false;
            Key k=(Key)other;
            return length==k.length && Arrays.equals(hash,k.hash);
        }
    }
    private static final class Stored {
        final long position;
        final int method, length, payloadLength;
        final byte[] hash;
        Stored(long position,int method,int length,int payloadLength,byte[] hash) {
            this.position=position;this.method=method;this.length=length;
            this.payloadLength=payloadLength;this.hash=hash;
        }
    }
    private static final class CountOut extends OutputStream {
        final OutputStream target;
        long bytes;
        CountOut(OutputStream target) { this.target=target; }
        @Override public void write(int b) throws IOException { target.write(b);bytes++; }
        @Override public void write(byte[] b,int offset,int length) throws IOException {
            target.write(b,offset,length);bytes+=length;
        }
        @Override public void flush() throws IOException { target.flush(); }
    }
    private static final class CountIn extends FilterInputStream {
        long bytes;
        CountIn(InputStream stream) { super(stream); }
        @Override public int read() throws IOException {
            int b=in.read();if(b>=0)bytes++;return b;
        }
        @Override public int read(byte[] b,int off,int len) throws IOException {
            int count=in.read(b,off,len);if(count>0)bytes+=count;return count;
        }
    }
    private static MessageDigest sha() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
    }
    private static String hex(byte[] b) {
        char[] chars="0123456789abcdef".toCharArray();
        StringBuilder result=new StringBuilder(b.length*2);
        for(byte v:b){int x=v&255;result.append(chars[x>>>4]).append(chars[x&15]);}
        return result.toString();
    }
    private static void checkCancel(Cancel cancel) throws IOException {
        if (cancel!=null && cancel.requested()) throw new IOException("Operación cancelada");
    }
    private static int nextBlock(InputStream source,byte[] block) throws IOException {
        int size=0;
        while(size<block.length){
            int n=source.read(block,size,block.length-size);
            if(n<0)break;
            if(n==0){int single=source.read();if(single<0)break;block[size++]=(byte)single;}
            else size+=n;
        }
        return size;
    }
    private static int period(byte[] block,int n) {
        for(int size=1;size<=MAX_PATTERN && size<=n;size++){
            boolean same=true;
            int small=Math.min(n,1024);
            for(int j=size;j<small;j++)if(block[j]!=block[j%size]){same=false;break;}
            if(!same)continue;
            for(int j=small;j<n;j++)if(block[j]!=block[j%size]){same=false;break;}
            if(same)return size;
        }
        return 0;
    }
    private static byte[] deflate(Deflater compressor,byte[] bytes,int length) {
        compressor.reset();
        compressor.setInput(bytes,0,length);
        compressor.finish();
        byte[] result=new byte[length+512];
        int size=0;
        while(!compressor.finished() && size<result.length){
            int n=compressor.deflate(result,size,result.length-size);
            if(n==0)break;
            size+=n;
        }
        return compressor.finished() ? Arrays.copyOf(result,size) : null;
    }

    public static Result compact(InputStream source,OutputStream target,long expected,
                                 Progress progress,Cancel cancel) throws IOException {
        if(source==null || target==null)throw new IllegalArgumentException("Falta origen o destino");
        CountOut counter=new CountOut(target);
        DataOutputStream out=new DataOutputStream(new BufferedOutputStream(counter,131072));
        out.write(MAGIC); out.writeInt(BLOCK);
        Map<Key,Integer> seen=new HashMap<>();
        MessageDigest complete=sha();
        MessageDigest perBlock=sha();
        Deflater compressor=new Deflater(6);
        byte[] buffer=new byte[BLOCK];
        long original=0,blocks=0,duplicate=0,zeros=0,patterns=0,compressed=0,raw=0;
        try{
            int count;
            while((count=nextBlock(source,buffer))!=0){
                checkCancel(cancel);
                if(Long.MAX_VALUE-original<count)throw new IOException("ISO demasiado grande");
                original+=count;blocks++;
                complete.update(buffer,0,count);
                int pattern=period(buffer,count);
                if(pattern==1 && buffer[0]==0) {
                    out.writeByte(ZERO);out.writeInt(count);zeros++;
                } else if(pattern>0 && pattern+9<count){
                    out.writeByte(PATTERN);out.writeInt(count);
                    out.writeInt(pattern);out.write(buffer,0,pattern);patterns++;
                } else {
                    perBlock.update(buffer,0,count);
                    byte[] digest=perBlock.digest();
                    Key key=new Key(digest,count);
                    Integer previous=seen.get(key);
                    if(previous!=null){
                        out.writeByte(REF);out.writeInt(previous);duplicate++;
                    }else{
                        if(seen.size()==Integer.MAX_VALUE)throw new IOException("Demasiados bloques únicos");
                        seen.put(key,seen.size());
                        byte[] zipped=deflate(compressor,buffer,count);
                        boolean useZip=zipped!=null && zipped.length+12<count;
                        out.writeByte(useZip?DEFLATE:RAW);
                        out.writeInt(count);
                        out.writeInt(useZip?zipped.length:count);
                        out.write(digest);
                        if(useZip){out.write(zipped);compressed++;}
                        else {out.write(buffer,0,count);raw++;}
                    }
                }
                if(progress!=null)progress.update(original,expected);
            }
            out.writeByte(END);
            out.writeLong(original);
            out.writeLong(blocks);
            out.writeInt(seen.size());
            byte[] digest=complete.digest();
            out.write(digest);
            out.flush();
            if(progress!=null)progress.update(original,expected);
            return new Result(original,counter.bytes,blocks,seen.size(),duplicate,zeros,
                              patterns,compressed,raw,hex(digest));
        }finally{
            compressor.end();
        }
    }

    private static int length(DataInputStream in) throws IOException {
        int n=in.readInt();
        if(n<1 || n>BLOCK)throw new IOException("Longitud de bloque inválida");
        return n;
    }
    private static byte[] shaOf(byte[] data) { return sha().digest(data); }
    private static byte[] decode(int method,byte[] payload,int expected) throws IOException {
        if(method==RAW) {
            if(payload.length!=expected)throw new IOException("Bloque sin comprimir inconsistente");
            return payload;
        }
        if(method!=DEFLATE)throw new IOException("Compresión desconocida");
        Inflater inflater=new Inflater();
        try {
            inflater.setInput(payload);
            byte[] value=new byte[expected];
            int written=0;
            while(written<expected && !inflater.finished()){
                int n=inflater.inflate(value,written,expected-written);
                if(n==0){
                    if(inflater.needsInput() || inflater.needsDictionary())
                        throw new IOException("Bloque comprimido incompleto");
                    throw new IOException("Bloque comprimido no progresa");
                }
                written+=n;
            }
            if(written!=expected)throw new IOException("Longitud descomprimida no coincide");
            if(!inflater.finished()){
                byte[] extra=new byte[1];
                if(inflater.inflate(extra)!=0 || !inflater.finished())
                    throw new IOException("Bloque excede su tamaño declarado");
            }
            if(inflater.getRemaining()!=0)throw new IOException("Datos sobrantes en bloque comprimido");
            return value;
        }catch(DataFormatException e){
            throw new IOException("Bloque comprimido corrupto",e);
        }finally{inflater.end();}
    }
    private static byte[] absoluteRead(FileChannel channel,long offset,int length) throws IOException {
        byte[] bytes=new byte[length];
        ByteBuffer b=ByteBuffer.wrap(bytes);
        while(b.hasRemaining()){
            int n=channel.read(b,offset+b.position());
            if(n<=0)throw new EOFException("Referencia a bloque truncado");
        }
        return bytes;
    }
    public static Result restore(FileChannel channel,OutputStream target,long total,
                                 Progress progress,Cancel cancel) throws IOException {
        if(channel==null || target==null)throw new IllegalArgumentException("Falta origen o destino");
        channel.position(0);
        CountIn counter=new CountIn(new BufferedInputStream(Channels.newInputStream(channel),131072));
        DataInputStream in=new DataInputStream(counter);
        byte[] magic=new byte[MAGIC.length];
        in.readFully(magic);
        if(!Arrays.equals(magic,MAGIC))throw new IOException("No es un archivo REVERSE ISO válido");
        if(in.readInt()!=BLOCK)throw new IOException("Versión o tamaño de bloque incompatible");
        List<Stored> entries=new ArrayList<>();
        MessageDigest full=sha();
        long rawLength=0,blocks=0,zeros=0,dupes=0,patterns=0,compressed=0,raw=0;
        while(true){
            checkCancel(cancel);
            int tag=in.readUnsignedByte();
            if(tag==END){
                long expectedBytes=in.readLong(),expectedBlocks=in.readLong();
                int expectedUnique=in.readInt();
                byte[] expectedHash=new byte[32];in.readFully(expectedHash);
                if(expectedBytes!=rawLength || expectedBlocks!=blocks || expectedUnique!=entries.size()
                        || !Arrays.equals(expectedHash,full.digest()))
                    throw new IOException("Integridad SHA-256 o longitud incorrecta: ISO reconstruida no coincide");
                if(counter.bytes!=channel.size())
                    throw new IOException("El archivo compacto contiene bytes adicionales");
                target.flush();
                if(progress!=null)progress.update(counter.bytes,total);
                return new Result(rawLength,counter.bytes,blocks,entries.size(),dupes,zeros,
                                  patterns,compressed,raw,hex(expectedHash));
            }
            byte[] reconstructed;
            if(tag==ZERO){
                int n=length(in);reconstructed=Arrays.copyOf(ZEROS,n);zeros++;
            }else if(tag==PATTERN){
                int n=length(in),p=in.readInt();
                if(p<1 || p>MAX_PATTERN || p>n)throw new IOException("Patrón inválido");
                byte[] motif=new byte[p];in.readFully(motif);
                reconstructed=new byte[n];
                for(int i=0;i<n;i++)reconstructed[i]=motif[i%p];
                patterns++;
            }else if(tag==REF){
                int id=in.readInt();
                if(id<0 || id>=entries.size())throw new IOException("Referencia a bloque inexistente");
                Stored item=entries.get(id);
                byte[] payload=absoluteRead(channel,item.position,item.payloadLength);
                reconstructed=decode(item.method,payload,item.length);
                if(!Arrays.equals(shaOf(reconstructed),item.hash))
                    throw new IOException("Referencia de bloque corrupta");
                dupes++;
            }else if(tag==RAW || tag==DEFLATE){
                int n=length(in);
                int size=in.readInt();
                if(size<1 || size>BLOCK+512 || (tag==RAW && size!=n))
                    throw new IOException("Tamaño de bloque compacto inválido");
                byte[] digest=new byte[32];in.readFully(digest);
                long position=counter.bytes;
                byte[] payload=new byte[size];in.readFully(payload);
                reconstructed=decode(tag,payload,n);
                if(!Arrays.equals(shaOf(reconstructed),digest))
                    throw new IOException("SHA-256 de bloque incorrecto");
                entries.add(new Stored(position,tag,n,size,digest));
                if(tag==RAW)raw++;else compressed++;
            }else throw new IOException("Marcador desconocido: "+tag);
            if(Long.MAX_VALUE-rawLength<reconstructed.length)
                throw new IOException("Longitud de ISO excedida");
            rawLength+=reconstructed.length;
            blocks++;
            full.update(reconstructed);
            target.write(reconstructed);
            if(progress!=null)progress.update(counter.bytes,total);
        }
    }
}

all :
	gcc -shared -fPIC -o mod_vad.so mod_vad.c -pthread -I /usr/local/freeswitch/include/freeswitch/ -I /home/zhangchao/freeswitch/src/include/ -lnsvad

clean:
	rm -f mod_vad.so

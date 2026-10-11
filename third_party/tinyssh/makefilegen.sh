#!/bin/sh

LANG=C
export LANG

(
  (
    echo "CC?=cc"
    echo "CFLAGS+=-W -Wall -Os -fPIC -fwrapv -Icryptoint"
    echo "LDFLAGS?="
    echo "CPPFLAGS?="
    echo
    echo "DESTDIR?="
    echo "PREFIX?=/usr/local"
    echo
    echo "INSTALL?=install"
    echo

    links='tinysshd-makekey tinysshd-printkey tinysshnoneauthd'
    autoheaders=''
    binaries=''
    objlib=''
    objall=''
    for file in `ls -1`; do
      ofile=`echo ${file} | sed 's/\.c$/.o/'`
      hfile=`echo ${file} | sed 's/\.c$/.h/'`
      case "${file}" in
        randombytes\.c)
          objall="${objall} ${ofile}"
        ;;
        has*\.c)
          autoheaders="${autoheaders} `echo ${file} | sed 's/\.c/.h/'`"
        ;;
        *\.c)
          if grep '^int main(' "${file}" >/dev/null; then
            binaries="${binaries} `echo ${file} | sed 's/\.c$//'`"
          else
            objlib="${objlib} ${ofile}"
          fi
          objall="${objall} ${ofile}"
        ;;
        *)
        ;;
      esac
    done


    echo "LINKS=${links}"
    echo

    echo "`echo BINARIES=${binaries} | fold -s | sed 's/^/ /' | sed 's/^ BINARIES= /BINARIES=/' | sed 's/ $/ \\\\/'`"
    echo

    echo "`echo OBJLIB=${objlib} | fold -s | sed 's/^/ /' | sed 's/^ OBJLIB= /OBJLIB=/' | sed 's/ $/ \\\\/'`"
    echo

    echo "`echo OBJALL=${objall} | fold -s | sed 's/^/ /' | sed 's/^ OBJALL= /OBJALL=/' | sed 's/ $/ \\\\/'`"
    echo

    echo "`echo AUTOHEADERS=${autoheaders} | fold -s | sed 's/^/ /' | sed 's/^ AUTOHEADERS= /AUTOHEADERS=/' | sed 's/ $/ \\\\/'`"
    echo

    echo "all: \$(AUTOHEADERS) \$(BINARIES) \$(LINKS)"
    echo

    for hfile in ${autoheaders}; do
      : > "${hfile}"
    done
    for ofile in ${objall}; do
      (
        cfile=`echo ${ofile} | sed 's/\.o/.c/'`
        gcc -MM -isystem /usr/local/include -Icryptoint "${cfile}"
        echo "	\$(CC) \$(CFLAGS) \$(CPPFLAGS) -c ${cfile}"
        echo
      )
    done
    rm -f ${autoheaders}

    for file in ${binaries}; do
      ofile="${file}.o"
      echo "${file}: ${ofile} \$(OBJLIB) randombytes.o libs"
      echo "	\$(CC) \$(CFLAGS) \$(CPPFLAGS) -o ${file} ${ofile} \\"
      echo "	\$(OBJLIB) \$(LDFLAGS) \`cat libs\`" randombytes.o
      echo
    done
    echo

    for hfile in ${autoheaders}; do
      cfile=`echo ${hfile} | sed 's/\.h/.c/'`
      lfile=`echo ${cfile} | sed 's/\.c/.log/'`
      touch "${hfile}"
      echo "${hfile}: tryfeature.sh ${cfile} libs"
      echo "	env CC=\"\$(CC)\" CFLAGS=\"\$(CFLAGS)\" LDFLAGS=\"\$(LDFLAGS) \`cat libs\`\" \\"
      echo "	./tryfeature.sh ${cfile} >${hfile} 2>${lfile}"
      echo "	cat ${hfile}"
      echo
    done

    echo "test:"
    echo "	\$(MAKE) -C tests test"
    echo

    echo "test-ssh:"
    echo "	\$(MAKE) -C tests test-ssh"
    echo

    echo "libs: trylibs.sh"
    echo "	env CC=\"\$(CC)\" ./trylibs.sh -lsocket -lnsl -lutil -lrandombytes -l25519 -l1305 -lntruprime >libs 2>libs.log"
    echo "	cat libs"
    echo

    echo "tinysshd-makekey: tinysshd"
    echo "	rm -f tinysshd-makekey"
    echo "	ln -s tinysshd tinysshd-makekey"
    echo

    echo "tinysshd-printkey: tinysshd"
    echo "	rm -f tinysshd-printkey"
    echo "	ln -s tinysshd tinysshd-printkey"
    echo

    echo "tinysshnoneauthd: tinysshd"
    echo "	rm -f tinysshnoneauthd"
    echo "	ln -s tinysshd tinysshnoneauthd"
    echo

    echo "install: \$(BINARIES) \$(LINKS)"
    echo "	mkdir -p \$(DESTDIR)\$(PREFIX)/sbin"
    echo "	mkdir -p \$(DESTDIR)\$(PREFIX)/share/man/man8"
    echo "	\$(INSTALL) -m 0755 tinysshd \$(DESTDIR)\$(PREFIX)/sbin/tinysshd"
    echo "	\$(INSTALL) -m 0755 tinysshd-makekey \$(DESTDIR)\$(PREFIX)/sbin/tinysshd-makekey"
    echo "	\$(INSTALL) -m 0755 tinysshd-printkey \$(DESTDIR)\$(PREFIX)/sbin/tinysshd-printkey"
    echo "	\$(INSTALL) -m 0755 tinysshnoneauthd \$(DESTDIR)\$(PREFIX)/sbin/tinysshnoneauthd"
    echo "	\$(INSTALL) -m 0644 man/tinysshd.8 \$(DESTDIR)\$(PREFIX)/share/man/man8/tinysshd.8"
    echo "	\$(INSTALL) -m 0644 man/tinysshd-makekey.8 \$(DESTDIR)\$(PREFIX)/share/man/man8/tinysshd-makekey.8"
    echo "	\$(INSTALL) -m 0644 man/tinysshd-printkey.8 \$(DESTDIR)\$(PREFIX)/share/man/man8/tinysshd-printkey.8"
    echo "	\$(INSTALL) -m 0644 man/tinysshnoneauthd.8 \$(DESTDIR)\$(PREFIX)/share/man/man8/tinysshnoneauthd.8"
    echo

    echo "clean:"
    echo "	\$(MAKE) -C tests clean"
    echo "	rm -f *.log libs \$(OBJLIB) \$(OBJALL) \$(BINARIES) \$(LINKS) \$(AUTOHEADERS)"
    echo 

  ) > Makefile
)

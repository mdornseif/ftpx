#!/usr/local/bin/python -*- Mode: Python; tab-width: 4 -*-

version = '$Id: ftpx.py,v 1.2 2002/01/18 07:38:39 drt Exp $'

# TODO:
# sort resp output
# generate statistics
# gnerate address lists

ftpuser = 'anonymous';
ftppass = 'drt-ftpfind@un.bewaff.net';
outputdir = 'filelists'

# max time we wait for a sucessfull data gathering process
timeout = 66

# number of concurrent querys this might be limited by your OS
# Win 95: 55, Linux 2.0: 245, Linux 2.2: 1000
# FreeBSD, NT: 1000; can be tweaked for more.
concurrency = 24

import sys
import socket
import time
import select
import asyncore
import asynchat
import ftpparse
import md5
import os
import os.path

def monitor():
    '''reap stale and open new connenctions until we reach concurrency'''

    # from work_in_progress/reaper.py
    # 'bring out your dead, <CLANG!>... bring out your dead!'
    now = int(time.time())
    for x in asyncore.socket_map.keys():
        s =  asyncore.socket_map[x]
        if hasattr(s, 'timestamp'):
            if (now - s.timestamp) > timeout:
                print >>sys.stderr, 'reaping connection to', s.host
                s.close()

    # create new connections
    while len(asyncore.socket_map) < concurrency:
        line = sys.stdin.readline()
        if line[-1:] == '\n':
            line = line [:-1]
        if line != '':
            s = ftpCommandChannel(line.strip())
            import gc
            gc.collect()
        else:
            break

def loop():
    '''loop over our sockets and monitor connections'''

    if hasattr (select, 'poll') and hasattr (asyncore, 'poll3'):
        poll_fun = asyncore.poll3
    else:
        poll_fun = asyncore.poll

    while asyncore.socket_map:
        monitor()
        poll_fun(30.0, asyncore.socket_map)


class ftpFileList:
    def __init__(self, host, port = 21, user = ftpuser, passwd = ftppass):
        self.host = host
        self.port = port
        self.user = user
        self.passwd = passwd
        self.dirs = {}
        self.dirdupes = {}

    def __repr__(self):
        return "%s, %r, %r" % (self.host, self.dirs, self.dirdupres)

    def __del__(self):
        servername = "%s,%s,%s,%s" % (self.host, self.port, self.user, self.passwd)
        filename = os.path.join(outputdir, servername)
        tmpname = os.path.join(outputdir, ".tmp.%s" % (servername))
        fd = open(tmpname, 'w')
        url = self.host
        if self.port != 21:
            url += ":%d" % self.port
        if self.user !='anonymous':
            url = '%s:%s@%s' % (self.user, self.passwd, url)
        fd.write("{'server': 'ftp://%s', 'dirs':, %r}\n" % (url, self.dirs))
        fd.close()
        os.rename(tmpname, filename)
        for x in self.dirs.keys():
            self.dirs[x] = None
            del(self.dirs[x])
        self.dirs.clear()
        self.dirs = None
        self.dirdupes = None

    def addDir(self, path, files):
        if len(files) > 0:
            hash = md5.new(repr(files)).digest()
            if self.dirdupes.has_key(hash):
                print path, "is identical to", self.dirdupes[hash], "- ignoring", files
            else:
                self.dirdupes[hash] = path
                self.dirs[path] = (int(time.time()), files)

                    
class ftpDataChannel(asynchat.async_chat):
    '''class implementing connection to a server to get data the PASV way'''

    def __init__(self, address, port, path, addtodo, addfiles):
        '''constuctor - opens connection'''
        asynchat.async_chat.__init__ (self)
        self.create_socket (socket.AF_INET, socket.SOCK_STREAM)
        self.set_terminator ('\r\n')
        self.buffer = ''
        self.path = path
        self.host = address
        self.port = port
        self.timestamp = int(time.time())
        self.addtodo = addtodo
        self.addfiles = addfiles

        # ensure path ends with "/"
        if self.path[-1] != '/':
            self.path += '/'
        
        try:
            self.connect((address, port))
        except:
            self.handle_error()
            self.close()

        # Set terminator to something preforming better
        
    def handle_connect(self):
        '''we have successfull connected'''
        # ... and ignore this fact
        pass 
               
    def handle_error(self):
        '''print out error information to stderr'''
        print >>sys.stderr, self.host, ": ERROR:", self, self.host, self.port, sys.exc_info()[1]
    
    def collect_incoming_data (self, data):
        '''collect data which was recived on the socket'''
        self.buffer = self.buffer + data
      
    def found_terminator (self):
        '''we have read a whole line and decide what do do next'''
        self.buffer += '\n'
        # update timestamp
        self.timestamp = int(time.time())

    def handle_close (self):
        '''when the connection is closed we print out the recived data'''
        self.close()
        files = []
        for x in ftpparse.ftpparse(self.buffer.split('\n')):
            if x != None:
                if x[ftpparse.CWD] == 1:
                    self.addtodo(self.path + x[ftpparse.NAME] + "/")
                if x[ftpparse.RETR] == 1 and x[ftpparse.SIZE] > 99:
                    files.append((x[ftpparse.NAME], x[ftpparse.SIZE], x[ftpparse.MTIME]))
        self.addfiles(self.path, files)
        self.addfiles = None
        self.addtodo = None

class ftpCommandChannel (asynchat.async_chat):
    '''class implementing the actual scan'''
    
    def __init__ (self, address):
        '''constuctor - opens connection'''
        asynchat.async_chat.__init__ (self)
        self.create_socket (socket.AF_INET, socket.SOCK_STREAM)
        self.set_terminator ('\r\n')
        self.buffer = ''
        self.host = address
        self.timestamp = int(time.time())
        self.state = 'WAIT220USER' 
        self.tododirs = ['/', '/pub']
        self.donedirs = []
        self.pwd = '/'
        self.filelist = ftpFileList(self.host)
        self.datachan = None

        try:
            self.connect((address, 21))
        except:
            self.handle_error()
            self.close()

    def handle_connect(self):
        '''we have successfull connected'''
        # ... and ignore this fact
        pass
               
    def handle_error(self):
        '''print out error information to stderr'''
        print >>sys.stderr, self.host, ": ERROR:", self.host, sys.exc_info()[1]
        
    def handle_close (self):
        '''when the connection is closed use monitor() to start new connections'''        
        self.close()
        self.filelist = None
        monitor()
               
    def push(self, data):
        asynchat.async_chat.push(self, data)
        if __debug__:
            print >>sys.stderr, self.host, ">>", data,
            sys.stderr.flush()
            
    def collect_incoming_data (self, data):
        '''collect data which was recived on the socket'''
        self.buffer = self.buffer + data
      
    def found_terminator (self):
        '''we have read a whole line and decide what do do next'''
        data = self.buffer
        self.buffer = ''
        # update timestamp
        self.timestamp = int(time.time())
        # save response
        if __debug__:
            print >>sys.stderr, self.host, "<<", repr(data), repr(self.state)
            sys.stderr.flush()

        if len(data) < 4:
            return

        # self.state is what we do NEXT 
        # check if the server awaits a new command
        if data[3] == ' ' and self.state == 'WAIT220USER':
            if data[:3] == '220':
                # send USER command
                self.push("USER %s\r\n" % (ftpuser))
                self.state = 'WAITx3xPASS'
            else:
                # TODO: catch banners
                print >>sys.stderr, "no successful greeting:", repr(data)
        elif data[3] == ' ' and self.state == 'WAITx3xPASS':
            # TODO: check for 530 Permission denied.
            if data[:3] == '230' or data[:3] == '331' or data[:3] == '332':
                # send PASS command
                self.push("PASS %s\r\n" % (ftppass))
                self.state = 'WAIT230PASV'
            else:
                print >>sys.stderr, "no successful USER:", repr(data)                
        elif data[3] == ' ' and self.state == 'WAIT230PASV':
            if data[:3] == '230' or data[:3] == '202':
                # send PASV command
                self.push("PASV\r\n")
                self.state = 'WAIT227CWD'
            else:
                print >>sys.stderr, "no successful PASS:", repr(data)                
        elif data[3] == ' ' and self.state == 'PASV':
            # send PASV command
            self.push("PASV\r\n")
            self.state = 'WAIT227CWD'
        elif data[:4] == '227 ' and self.state == 'WAIT227CWD':
            # wait for 227 response, parse it and send cwd
            self.parsePasv(data)
            if len(self.tododirs) > 0:
                self.pwd = self.tododirs.pop(0)
                self.push("CWD %s\r\n" % (self.pwd))
                self.state = 'WAIT250PWD'
            else:
                self.push("QUIT\r\n")
                self.state == 'FIN'
        elif data[3] == ' ' and self.state == 'WAIT250PWD':
            if data[:3] == '250':
                # send LIST command
                self.push("PWD\r\n")
                self.state = 'WAIT257LIST'
            else:
                print >>sys.stderr, "CWD unsuccessful", self.pwd
                # cwd unsuccessful, next dir
                self.push("PASV\r\n")
                self.state = 'WAIT227CWD'
        elif data[3] == ' ' and self.state == 'WAIT257LIST':
            if data[:3] == '257' and (data.split('"')[1] == self.pwd or data.split('"')[1] == self.pwd[:-1]):
                # send LIST command
                if self.datachan:
                    del(self.datachan)
                self.datachan = ftpDataChannel(self.host, self.pasvport, self.pwd, self.addTodo, self.filelist.addDir)
                self.push("LIST\r\n")
                self.state = 'WAIT150'
            else:
                print >>sys.stderr, "CWD unsuccessful", self.pwd, "leads to", data.split('"')[1]
                # cwd unsuccessful, next dir
                self.push("PASV\r\n")
                self.state = 'WAIT227CWD'
        elif data[3] == ' ' and self.state == 'WAIT150':
            if data[:3] == '150':
                self.state = 'WAIT226'
            else:
                self.push("PASV\r\n")
                self.state = 'WAIT227CWD' 
        elif data[:4] == '226 ' and self.state == 'WAIT226':
            # wait for successful data transfer
            self.donedirs.append(self.pwd)
            self.push("PASV\r\n")
            self.state = 'WAIT227CWD'
        elif data[3] == '-':
            pass
        elif data[3] == ' ' and self.state == 'QUIT':
            self.push("QUIT\r\n")
            self.state == 'FIN'


    def parsePasv(self, data):
        addr = 'X'
        for x in data:
            if x.isdigit() and addr[0] == 'X':
                addr = ''
            addr += x
        addrlist = addr.split(',')
        p1 = addrlist[4]
        p2 = ''
        for x in addrlist[5]:
            if not x.isdigit():
                break
            p2 += x
        self.pasvport = ((int(p1) % 256) * 256) + (int(p2) % 256)
        

    def addTodo(self, dirname):
        if dirname not in self.donedirs:
            if dirname not in self.tododirs:
                if dirname != self.pwd:
                    if dirname.count('/') < 16:
                        self.tododirs.append(dirname)
        else:
            print "ignored", dirname
                    

import gc
gc.set_debug(gc.DEBUG_LEAK)
gc.collect()
# "main"
# use monitor() to fire up the number of connections we want
monitor()
# handle all the connection stuff
loop()



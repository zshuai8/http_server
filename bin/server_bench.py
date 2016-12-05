#!/usr/bin/python

#
# Benchmark the sysstatd project
#
#
# @author gback; Spring 2016
#

import getopt, sys, os, subprocess, signal, re, json, resource, time, socket, atexit, httplib
from collections import namedtuple

runconfig = namedtuple('runconfig', ['name',        # name of configuration
                         'nthreads',    # number of threads
                         'nconnections',# number of conn per thread
                         'description', # description
                         'path',        # target path to be retrieved
                         'duration'])   # duration (with unit as string, i.e. "1s")

VERSION="1.0"
server_exe = "./sysstatd"
server_root = "_serverroot_"
wrk_exe = "/home/courses/cs3214/bin/wrk"

# tests will be run in this order
tests = [
    runconfig(name="loadavg40", nthreads=20, nconnections=40, duration="10s", path="/loadavg",
        description="""
    Can your server handle 40 parallel connections request /loadavg?
    """),
    runconfig(name="loadavg500", nthreads=20, nconnections=500, duration="10s", path="/loadavg",
        description="""
    Using 500 connections, each of which is repeatedly requesting /loadavg (~80bytes in
    HTTP body).  We believe this should be enough to make the server CPU bound.
    """),
    runconfig(name="loadavg10k", nthreads=20, nconnections=10000, duration="10s", path="/loadavg",
        description="""
    Handling 10k simultaneous connections has been a target of scalability since 1999:
    http://www.kegel.com/c10k.html
    Can your server handle it?
    """),
    runconfig(name="wwwcsvt100", nthreads=20, nconnections=100, duration="10s", path="/files/www.cs.vt.edu-20160222.html",
        description="""
    The home page of the CS Department, as of 4/22/2016, is about 23KB large (not counting embedded objects).
    If 100 clients accessed it simultaneously, how much throughput could they expect?
    """),
    runconfig(name="doom100", nthreads=20, nconnections=40, duration="10s", path="/files/large",
        description="""
    According to https://mobiforge.com/research-analysis/the-web-is-doom the combined size of all
    objects that make an average web page is 2,250kBytes as of April 2016. If these were transferred
    all in a single objects, how much throughput would you get?
    This should max out the 10Gbps Ethernet links, even with only 40 connections.
    """)
]
testsbyname = dict((c.name, c) for c in tests)
teststorun = map(lambda t : t.name, tests)

def listtests():
    for test in tests:
        print """
Test:           %s
Connections:    %d
Duration:       %s
Path:           %s
Description:    %s
""" % (test.name, test.nconnections, test.duration, test.path, test.description)


script_dir = "/".join(os.path.realpath(__file__).split("/")[:-1])
if script_dir == "":
    script_dir = "."

script_dir = os.path.realpath(script_dir)

def usage():
    print """
Usage: %s [-hv] [-l] [-s server] [-R serverroot] [-t test1,test2,...] [url]

   -h                     display this help
   -v                     run verbose
   -s                     path to server executable, default %s
   -R server_root         path to server root, default %s
   -t test                run just the tests specified
   -l                     list available tests with their descriptions
   url                    URL where your server can be reached, i.e. 
                            http://hickory.rlogin:12306/

This script must be started on two different rlogin nodes.
On the first node, run it without a URL to start the server.

Then run it on a second node with the URL printed out by the
first run.

    """ % (sys.argv[0], server_exe, server_root, config)

try:
    opts, args = getopt.getopt(sys.argv[1:], "hvs:R:t:l", ["help", "verbose"])
except getopt.GetoptError, err:
    print str(err)
    usage()
    sys.exit(2)

verbose = False
hostname = socket.gethostname()

for opt, arg in opts: 
    if opt == "-h":
        usage()
        sys.exit(0)
    if opt == "-v":
        verbose = True
    elif opt == "-s":
        server_exe = arg
    elif opt == "-R":
        server_root = arg
    elif opt == "-l":
        listtests()
        sys.exit(0)
    elif opt == "-t":
        teststorun = arg.split(",")
    else:
        assert False, "unhandled option"

def raise_fd_limit():
    print "I will now try to raise the file descriptor limit"
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    print "Your server process can open %d file descriptors simultaneously." % soft

#
# Start the server.
#
def start_server(root_dir):

    print "I will now prepare your server for benchmarking."
    if not os.access(server_exe, os.X_OK):
        print "Did not find server executable: %s" % (server_exe)
        sys.exit(-1)

    # prepare files to be served
    print "I will use the directory %s to store 2 files" % (root_dir)
    if not os.access(root_dir, os.W_OK):
        os.mkdir(root_dir)

    def make_synthetic_content(sz):
        return '0123456789ABCDEF' * (sz/16)

    def write_file(name, content):
        with open("%s/%s" % (root_dir, name), "w") as sfile:
            sfile.write(content)
            sfile.close()

    sfilecontent = make_synthetic_content(1024)
    write_file("small", sfilecontent)
    lfilecontent = make_synthetic_content(2250*1024)
    write_file("large", lfilecontent)
    wwwcscont = open("%s/res/www.cs.vt.edu-20160222.html" % script_dir).read()
    write_file("www.cs.vt.edu-20160222.html", wwwcscont)

    port = (os.getpid() % 10000) + 20000

    cmd = [server_exe,"-p", str(port), "-R", root_dir, '-s']

    raise_fd_limit()

    server = subprocess.Popen(cmd, stdout=open(os.devnull, "w"), stderr=sys.stderr)

    def clean_up_testing():
        try:
            os.kill(server.pid, signal.SIGKILL)
        except:
            pass

    atexit.register(clean_up_testing)

    print "I will now test that your server works."
    def test_server():
        http_conn = httplib.HTTPConnection(hostname, port)
        http_conn.connect()
        for url, expected in zip(["/files/small", "/files/large", "/files/www.cs.vt.edu-20160222.html", "/loadavg", "/meminfo"], 
                                 [sfilecontent, lfilecontent, wwwcscont, 10, 100]):
            http_conn.request("GET", url)
            server_response = http_conn.getresponse()
            sfile = server_response.read()
            if server_response.status != httplib.OK:
                print "Server returned %s for %s, expected %d." % (server_response.status, url, httplib.OK)
                sys.exit(-1)

            if isinstance(expected, int) and len(sfile) >= expected or \
               isinstance(expected, basestring) and sfile == expected:
                print "Retrieved %s ok." % (url)
            else:
                print "Did not find expected content at %s." % (url)
                sys.exit(-1)

        http_conn.close()

    for tries in range(10):
        try:
            time.sleep(1)
            test_server() 
            break
        except Exception as e:
            pass

    if tries == 9:
        print "Your server did not start, giving up after 10 tries"
        sys.exit(0)

    print """
Congratulations, you are now ready to run the benchmark!
Now, find another unloaded rlogin machine and run:

%s http://%s:%d/

When you are done, don't forget to hit ^C here.

Your server's stdout is going to /dev/null.
Your server's stderr is going to the driver's stderr.  
""" % (os.path.realpath(sys.argv[0]), hostname, port)

    server.wait()

def start_wrk(url, test):
    cmd = [wrk_exe,'-c',str(test.nconnections),
                   '-t',str(test.nthreads),
                   '-d',test.duration,
                   '-s', script_dir + "/cs3214bench.lua", url + test.path]

    if verbose:
        print "I will now run", " ".join(cmd)

    resfile = "ssresults.json"
    luajson = "%s/JSON.lua" % (script_dir)
    assert os.access(luajson, os.R_OK)
    server = subprocess.Popen(cmd, stdout=sys.stdout, stderr=sys.stderr,
        env=dict(os.environ, JSON_OUTPUT_FILE=resfile, JSON_LUA=luajson))
    server.wait()
    with open(resfile) as jfile:
        r = json.load(jfile)
        os.unlink(resfile)
        return r

if len(args) == 0:
    start_server(server_root)
else:
    url = args[0]
    # strip ending / since the path args contain them
    while url.endswith("/"):
        url = url[:-1]

    if hostname in url:
        print "Please do not start the client on the same machine as the server."
        sys.exit(-1)

    raise_fd_limit()
    results = dict(version = VERSION)
    for testname in teststorun:
        if testname not in testsbyname:
            print "Test: %s not found, skipping" % testname
            continue

        test = testsbyname[testname]
        print "Now running test: %s\n" % (testname)
        try:
            results[testname] = start_wrk(url, test)
        except Exception as e:
            print "An exception occurred %s, skipping this test" % (str(e)) 

    ofilename = "sysstatd.results.%d.json" % (os.getpid())
    print "Writing results to %s" % ofilename
    with open(ofilename, "w") as ofile:
        json.dump(results, ofile)

    print """
    Submit your results to the scoreboard with ~cs3214/bin/sspostresults.py %s
    """ % (ofilename)


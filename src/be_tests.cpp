// https://github.com/catchorg/Catch2/blob/master/docs/tutorial.md#top

#include <cstring>
#include <iostream>
#include <memory>
#include <cstdio>
#include <stdexcept>
#include <unistd.h>

#define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_CONSOLE_WIDTH 120

#include "config.h"

#ifdef HAVE_MACH_O_DYLD_H
#include "mach-o/dyld.h"         // Needed for _NSGetExecutablePath
#endif

#include "dfxml_cpp/src/dfxml_writer.h"
#include "be13_api/catch.hpp"
#include "be13_api/scanner_set.h"
#include "be13_api/utils.h"             // needs config.h

#include "base64_forensic.h"
#include "bulk_extractor_scanners.h"
#include "exif_reader.h"
#include "image_process.h"
#include "jpeg_validator.h"
#include "phase1.h"
#include "sbuf_decompress.h"
#include "scan_base64.h"
#include "scan_email.h"
#include "scan_msxml.h"
#include "scan_pdf.h"
#include "scan_vcard.h"
#include "scan_wordlist.h"

const std::string JSON1 {"[{\"1\": \"one@company.com\"}, {\"2\": \"two@company.com\"}, {\"3\": \"two@company.com\"}]"};
const std::string JSON2 {"[{\"1\": \"one@base64.com\"}, {\"2\": \"two@base64.com\"}, {\"3\": \"three@base64.com\"}]\n"};

std::filesystem::path test_dir()
{
#ifdef HAVE__NSGETEXECUTABLEPATH
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0){
        return std::filesystem::path(path).parent_path() / "tests";
    }
    throw std::runtime_error("_NSGetExecutablePath failed???\n");
#else
    return std::filesystem::canonical("/proc/self/exe").parent_path() / "tests";
#endif
}

sbuf_t *map_file(std::filesystem::path p)
{
    return sbuf_t::map_file( test_dir() / p );
}


/* Read all of the lines of a file and return them as a vector */
std::vector<std::string> getLines(const std::filesystem::path path)
{
    std::vector<std::string> lines;
    std::string line;
    std::ifstream inFile;
    inFile.open( path );
    if (!inFile.is_open()) {
        std::cerr << "getLines: Cannot open file: " << path << "\n";
        std::string cmd("ls -l " + path.parent_path().string());
        std::cerr << cmd << "\n";
        if (system( cmd.c_str())) {
            std::cerr << "error\n";
        }
        throw std::runtime_error("be_tests:getLines");
    }
    while (std::getline(inFile, line)){
        if (line.size()>0){
            lines.push_back(line);
        }
    }
    return lines;
}

/* Requires that a feature in a set of lines */
bool requireFeature(const std::vector<std::string> &lines, const std::string feature)
{
    for (const auto &it : lines) {
        if ( it.find(feature) != std::string::npos) return true;
    }
    std::cerr << "feature not found: " << feature << "\nfeatures found (perhaps one of these is the feature you are looking for?):\n";
    for (const auto &it : lines) {
        std::cerr << "  " << it << "\n";
    }
    return false;
}

/* Setup and run a scanner. Return the output directory */
std::vector<scanner_config::scanner_command> enable_all_scanners = {
    scanner_config::scanner_command(scanner_config::scanner_command::ALL_SCANNERS,
                                    scanner_config::scanner_command::ENABLE)
};


std::filesystem::path test_scanners(const std::vector<scanner_t *> & scanners, sbuf_t *sbuf)
{
    REQUIRE(sbuf->children == 0);

    const feature_recorder_set::flags_t frs_flags;
    scanner_config sc;
    sc.outdir           = NamedTemporaryDirectory();
    sc.scanner_commands = enable_all_scanners;

    scanner_set ss(sc, frs_flags, nullptr);
    for (auto const &it : scanners ){
        ss.add_scanner( it );
    }
    ss.apply_scanner_commands();

    REQUIRE (ss.get_enabled_scanners().size() == scanners.size()); // the one scanner
    std::cerr << "\n## output in " << sc.outdir << " for " << ss.get_enabled_scanners()[0] << "\n";
    REQUIRE(sbuf->children == 0);
    ss.phase_scan();
    REQUIRE(sbuf->children == 0);
    ss.process_sbuf(sbuf);
    ss.shutdown();
    return sc.outdir;
}

std::filesystem::path test_scanner(scanner_t scanner, sbuf_t *sbuf)
{
    // I couldn't figure out how to pass a vector of scanner_t objects...
    std::vector<scanner_t *>scanners = {scanner };
    return test_scanners(scanners, sbuf);
}


TEST_CASE("base64_forensic", "[support]") {
    const char *encoded="SGVsbG8gV29ybGQhCg==";
    const char *decoded="Hello World!\n";
    unsigned char output[64];
    size_t result = b64_pton_forensic(encoded, strlen(encoded), output, sizeof(output));
    REQUIRE( result == strlen(decoded) );
    REQUIRE( strncmp( (char *)output, decoded, strlen(decoded))==0 );
}

TEST_CASE("scan_base64_functions", "[support]" ){
    base64array_initialize();
    sbuf_t sbuf1("W3siMSI6ICJvbmVAYmFzZTY0LmNvbSJ9LCB7IjIiOiAidHdvQGJhc2U2NC5jb20i");
    bool found_equal = false;
    REQUIRE(sbuf_line_is_base64(sbuf1, 0, sbuf1.bufsize, found_equal) == true);
    REQUIRE(found_equal == false);

    sbuf_t sbuf2("W3siMSI6ICJvbmVAYmFzZTY0LmNvbSJ9LCB7IjIiOiAidHdvQGJhc2U2NC5jb20i\n"
                 "fSwgeyIzIjogInRocmVlQGJhc2U2NC5jb20ifV0K");
    REQUIRE(sbuf_line_is_base64(sbuf2, 0, sbuf1.bufsize, found_equal) == true);
    REQUIRE(found_equal == false);

    sbuf_t *sbuf3 = decode_base64(sbuf2, 0, sbuf2.bufsize);
    REQUIRE(sbuf3 != nullptr);
    REQUIRE(sbuf3->bufsize == 78);
    REQUIRE(sbuf3->asString() == JSON2);
    delete sbuf3;
}

/* scan_email.flex checks */
TEST_CASE("scan_email", "[support]") {
    {
        REQUIRE( extra_validate_email("this@that.com")==true);
        REQUIRE( extra_validate_email("this@that..com")==false);
        auto s1 = sbuf_t("this@that.com");
        auto s2 = sbuf_t("this_that.com");
        REQUIRE( find_host_in_email(s1) == 5);
        REQUIRE( find_host_in_email(s2) == -1);

        auto s3 = sbuf_t("https://domain.com/foobar");
        size_t domain_len = 0;
        REQUIRE( find_host_in_url(s3, &domain_len)==8);
        REQUIRE( domain_len == 10);
    }

    {
        /* This is text from a PDF, decompressed */
        auto *sbufp = new sbuf_t("q Q q 72 300 460 420 re W n /Gs1 gs /Cs1 cs 1 sc 72 300 460 420re f 0 sc./Gs2 gs q 1 0 0 -1 72720 cm BT 10 0 0 -10 5 10 Tm /F1.0 1 Tf (plain_text_pdf@textedit.com).Tj ET Q Q");
        auto outdir = test_scanner(scan_email, sbufp);
        auto email_txt = getLines( outdir / "email.txt" );
        REQUIRE( requireFeature(email_txt,"135\tplain_text_pdf@textedit.com"));
    }

    {
        auto *sbufp = new sbuf_t("plain_text_pdf@textedit.com");
        auto outdir = test_scanner(scan_email, sbufp);
        auto email_txt = getLines( outdir / "email.txt" );
        REQUIRE( requireFeature(email_txt,"0\tplain_text_pdf@textedit.com"));
    }

    {
        std::vector<scanner_t *>scanners = {scan_email, scan_pdf };
        auto *sbufp = map_file("nps-2010-emails.100k.raw");
        auto outdir = test_scanners(scanners, sbufp);
        auto email_txt = getLines( outdir / "email.txt" );
        REQUIRE( requireFeature(email_txt,"80896\tplain_text@textedit.com"));
        REQUIRE( requireFeature(email_txt,"70727-PDF-0\tplain_text_pdf@textedit.com\t"));
        REQUIRE( requireFeature(email_txt,"81991-PDF-0\trtf_text_pdf@textedit.com\t"));
        REQUIRE( requireFeature(email_txt,"92231-PDF-0\tplain_utf16_pdf@textedit.com\t"));
    }
}

TEST_CASE("sbuf_decompress_zlib_new", "[support]") {
    auto *sbufp = map_file("test_hello.gz");
    REQUIRE( sbuf_decompress::is_gzip_header( *sbufp, 0) == true);
    REQUIRE( sbuf_decompress::is_gzip_header( *sbufp, 10) == false);
    auto *decomp = sbuf_decompress::sbuf_new_decompress( *sbufp, 1024*1024, "GZIP", sbuf_decompress::mode_t::GZIP, 0 );
    REQUIRE( decomp != nullptr);
    REQUIRE( decomp->asString() == "hello@world.com\n");
    delete decomp;
    delete sbufp;
}

TEST_CASE("scan_exif", "[scanners]") {
    auto *sbufp = map_file("1.jpg");
    REQUIRE( sbufp->bufsize == 7323 );
    auto res = jpeg_validator::validate_jpeg(*sbufp);
    REQUIRE( res.how == jpeg_validator::COMPLETE );
    delete sbufp;
}

TEST_CASE("scan_msxml","[scanners]") {
    auto *sbufp = map_file("KML_Samples.kml");
    std::string bufstr = msxml_extract_text(*sbufp);
    REQUIRE( bufstr.find("http://maps.google.com/mapfiles/kml/pal3/icon19.png") != std::string::npos);
    REQUIRE( bufstr.find("A collection showing how easy it is to create 3-dimensional") != std::string::npos);
    delete sbufp;
}

TEST_CASE("scan_pdf", "[scanners]") {
    auto *sbufp = map_file("pdf_words2.pdf");
    pdf_extractor pe(*sbufp);
    pe.find_streams();
    REQUIRE( pe.streams.size() == 4 );
    REQUIRE( pe.streams[1].stream_start == 2214);
    REQUIRE( pe.streams[1].endstream_tag == 4827);
    pe.decompress_streams_extract_text();
    REQUIRE( pe.texts.size() == 1 );
    REQUIRE( pe.texts[0].txt.substr(0,30) == "-rw-r--r--    1 simsong  staff");
    delete sbufp;
}


TEST_CASE("scan_json1", "[scanners]") {
    /* Make a scanner set with a single scanner and a single command to enable all the scanners.
     */
    auto *sbufp = new sbuf_t("hello {\"hello\": 10, \"world\": 20, \"another\": 30, \"language\": 40} world");
    auto outdir = test_scanner(scan_json, sbufp); // delete sbufp

    /* Read the output */
    auto json_txt = getLines( outdir / "json.txt" );
    auto last = json_txt[json_txt.size()-1];

    REQUIRE(last.substr( last.size() - 40) == "6ee8c369e2f111caa9610afc99d7fae877e616c9");
    REQUIRE(true);
}

TEST_CASE("scan_net", "[scanners]") {
//TODO: Add checks for IPv4 and IPv6 header checksumers
}

TEST_CASE("scan_vcard", "[scanners]") {
    /* Make a scanner set with a single scanner and a single command to enable all the scanners.
     */
    auto *sbufp = map_file( "john_jakes.vcf" );
    auto outdir = test_scanner(scan_vcard, sbufp); // deletes sbuf2

    /* Read the output */
}

TEST_CASE("scan_wordlist", "[scanners]") {
    /* Make a scanner set with a single scanner and a single command to enable all the scanners.
     */
    auto *sbufp = map_file( "john_jakes.vcf" );
    auto outdir = test_scanner(scan_wordlist, sbufp); // deletes sbufp

    /* Read the output */
    auto wordlist_txt = getLines( outdir / "wordlist_dedup_1.txt");
    REQUIRE( wordlist_txt[0] == "States" );
    REQUIRE( wordlist_txt[1] == "America" );
    REQUIRE( wordlist_txt[2] == "Company" );
}

TEST_CASE("scan_zip", "[scanners]") {
    std::vector<scanner_t *>scanners = {scan_email, scan_zip };
    auto *sbufp = map_file( "testfilex.docx" );
    auto outdir = test_scanners( scanners, sbufp); // deletes sbuf2
    auto email_txt = getLines( outdir / "email.txt" );
    REQUIRE( requireFeature(email_txt,"1771-ZIP-402\tuser_docx@microsoftword.com"));
    REQUIRE( requireFeature(email_txt,"2396-ZIP-1012\tuser_docx@microsoftword.com"));
}


struct Check {
    Check(std::string fname_, Feature feature_):
        fname(fname_),
        feature(feature_) {};
    std::string fname;
    Feature feature;
};

TEST_CASE("test_validate", "[phase1]" ) {
    scanner_config sc;

    sc.outdir = NamedTemporaryDirectory();
    sc.scanner_commands = enable_all_scanners;
    const feature_recorder_set::flags_t frs_flags;

    auto *xreport = new dfxml_writer(sc.outdir / "report.xml", false);

    scanner_set ss(sc, frs_flags, xreport);
    ss.add_scanners(scanners_builtin);
    ss.apply_scanner_commands();
    ss.phase_scan();
    ss.shutdown();
    delete xreport;
}



/*
 * Run all of the built-in scanners on a specific image, look for the given features, and return the directory.
 */
std::string validate(std::string image_fname, std::vector<Check> &expected)
{
    std::cerr << "================ validate  " << image_fname << " ================\n";
    scanner_config sc;

    sc.outdir = NamedTemporaryDirectory();
    sc.scanner_commands = enable_all_scanners;
    const feature_recorder_set::flags_t frs_flags;
    auto *xreport = new dfxml_writer(sc.outdir / "report.xml", false);
    scanner_set ss(sc, frs_flags, xreport);
    ss.add_scanners(scanners_builtin);
    ss.apply_scanner_commands();

    if (image_fname != "" ) {
        auto p = image_process::open( test_dir() / image_fname, false, 65536, 65536);
        Phase1::Config cfg;  // config for the image_processing system
        Phase1 phase1(cfg, *p, ss);
        phase1.dfxml_write_create( 0, nullptr);

        ss.phase_scan();
        phase1.phase1_run();
        delete p;
    }
    ss.shutdown();

    xreport->pop("dfxml");
    xreport->close();
    delete xreport;


    for (size_t i=0; i<expected.size(); i++){
        std::filesystem::path fname  = sc.outdir / expected[i].fname;
        std::cerr << "---- " << i << " -- " << fname.string() << " ----\n";
        bool found = false;
        for (int pass=0 ; pass<2 && !found;pass++){
            std::string line;
            std::ifstream inFile;
            if (pass==1) {
                std::cerr << fname << ":\n";
            }
            inFile.open(fname);
            if (!inFile.is_open()) {
                throw std::runtime_error("validate_scanners:[phase1] Could not open "+fname.string());
            }
            while (std::getline(inFile, line)) {
                if (pass==1) {
                    std::cerr << line << "\n"; // print the file the second time through
                }
                auto words = split(line, '\t');
                if (words.size()==3 &&
                    words[0]==expected[i].feature.pos &&
                    words[1]==expected[i].feature.feature &&
                    words[2]==expected[i].feature.context){
                    found = true;
                    break;
                }
            }
        }
        if (!found){
            std::cerr << fname << " did not find " << expected[i].feature.pos
                      << " " << expected[i].feature.feature << " " << expected[i].feature.context << "\t";
        }
        REQUIRE(found);
    }
    std::cerr << "--- done ---\n\n";
    return sc.outdir;
}




TEST_CASE("test_json", "[phase1]") {
    std::vector<Check> ex1 {
        Check("json.txt",
              Feature( "0",
                       JSON1,
                       "ef2b5d7ee21e14eeebb5623784f73724218ee5dd")),
    };
    validate("test_json.txt", ex1);
}

TEST_CASE("test_base16json", "[phase1]") {
    std::vector<Check> ex2 {
        Check("json.txt",
              Feature( "50-BASE16-0",
                       "[{\"1\": \"one@base16_company.com\"}, "
                       "{\"2\": \"two@base16_company.com\"}, "
                       "{\"3\": \"two@base16_company.com\"}]",
                       "41e3ec783b9e2c2ffd93fe82079b3eef8579a6cd")),

        Check("email.txt",
              Feature( "50-BASE16-8",
                       "one@base16_company.com",
                       "[{\"1\": \"one@base16_company.com\"}, {\"2\": \"two@b")),

    };
    validate("test_base16json.txt", ex2);
}

TEST_CASE("test_hello", "[phase1]") {
    std::vector<Check> ex3 {
        Check("email.txt",
              Feature( "0-GZIP-0",
                       "hello@world.com",
                       "hello@world.com\\x0A"))

    };
    validate("test_hello.gz", ex3);
}

TEST_CASE("KML_Samples.kml","[phase1]"){
    std::vector<Check> ex4 {
        Check("kml.txt",
              Feature( "0",
                       "kml/000/0.kml",
                       "<fileobject><filename>kml/000/0.kml</filename><filesize>35919</filesize><hashdigest type='sha1'>"
                       "cffc78e27ac32414b33d595a0fefcb971eaadaa3</hashdigest></fileobject>"))
    };
    validate("KML_Samples.kml", ex4);
}

sbuf_t *make_sbuf()
{
    auto sbuf = new sbuf_t("Hello World!");
    return sbuf;
}

/* Test that sbuf data  are not copied when moved to a child.*/
std::atomic<int> counter{0};
const uint8_t *sbuf_buf_loc = nullptr;
void test_process_sbuf(sbuf_t *sbuf)
{
    if (sbuf_buf_loc != nullptr) {
        REQUIRE( sbuf_buf_loc == sbuf->get_buf() );
    }
    delete sbuf;
}

TEST_CASE("sbuf_no_copy", "[threads]") {
    for(int i=0;i<100;i++){
        auto sbuf = make_sbuf();
        sbuf_buf_loc = sbuf->get_buf();
        test_process_sbuf(sbuf);
    }
}

#if 0
/* Make the sbufs in the primary thread and dispose of them in the worker thread */
TEST_CASE("threadpool3", "[threads]") {
    counter = 0;
    class thread_pool pool;
    for(int i=0;i<100;i++){
        auto sbuf = make_sbuf();
        pool.push_task( [sbuf]{ test_process_sbuf(sbuf); } );
    }
    pool.wait_for_tasks();
    REQUIRE( counter==1000 );
}
#endif

/****************************************************************/
TEST_CASE("image_process", "[phase1]") {
    image_process *p = nullptr;
    REQUIRE_THROWS_AS( p = image_process::open( "no-such-file", false, 65536, 65536), image_process::NoSuchFile);
    REQUIRE_THROWS_AS( p = image_process::open( "no-such-file", false, 65536, 65536), image_process::NoSuchFile);
    p = image_process::open( test_dir() / "test_json.txt", false, 65536, 65536);
    REQUIRE( p != nullptr );
    int times = 0;

    for(auto it = p->begin(); it!=p->end(); ++it){
        REQUIRE( times==0 );
        sbuf_t *sbufp = it.sbuf_alloc();

        REQUIRE( sbufp->bufsize == 79 );
        REQUIRE( sbufp->pagesize == 79 );
        delete sbufp;
        times += 1;
    }
    REQUIRE(times==1);
    delete p;
}


#if 0
TEST_CASE("get_sbuf", "[phase1]") {
    image_process *p = image_process::open( image_fname, opt_recurse, cfg.opt_pagesize, cfg.opt_marginsize);
}
#endif

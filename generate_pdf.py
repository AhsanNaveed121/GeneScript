import os
from reportlab.lib.pagesizes import letter
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, Preformatted
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib import colors

def build_pdf(filename="README.pdf"):
    doc = SimpleDocTemplate(
        filename,
        pagesize=letter,
        rightMargin=40, leftMargin=40,
        topMargin=40, bottomMargin=40
    )

    styles = getSampleStyleSheet()

    title_style = ParagraphStyle(
        'DocTitle',
        parent=styles['Heading1'],
        fontSize=20,
        leading=24,
        textColor=colors.HexColor('#1E3A8A'),
        spaceAfter=6
    )
    
    sub_title_style = ParagraphStyle(
        'SubTitle',
        parent=styles['Normal'],
        fontSize=12,
        leading=16,
        textColor=colors.HexColor('#374151'),
        spaceAfter=14
    )

    h1_style = ParagraphStyle(
        'Heading1_Custom',
        parent=styles['Heading2'],
        fontSize=14,
        leading=18,
        textColor=colors.HexColor('#1E40AF'),
        spaceBefore=12,
        spaceAfter=6
    )

    body_style = ParagraphStyle(
        'Body_Custom',
        parent=styles['Normal'],
        fontSize=10,
        leading=14,
        textColor=colors.HexColor('#1F2937'),
        spaceAfter=6
    )

    code_style = ParagraphStyle(
        'Code_Custom',
        parent=styles['Code'],
        fontName='Courier',
        fontSize=8.5,
        leading=11.5,
        textColor=colors.HexColor('#111827'),
        backColor=colors.HexColor('#F3F4F6'),
        spaceBefore=4,
        spaceAfter=6
    )

    story = []

    # Title & Subtitle
    story.append(Paragraph("GeneScript: Compiler Construction Project", title_style))
    story.append(Paragraph("<b>Course:</b> Compiler Construction (BCS-7G) &nbsp;|&nbsp; <b>Instructor:</b> Sir Faisal Aslam", sub_title_style))

    # Team Members Table
    team_data = [
        ["Team Member", "Roll Number"],
        ["Eman Aamir", "23L-0612"],
        ["Haleema Sadia", "23L-0960"],
        ["Ahsan Naveed", "23L-0756"]
    ]
    t = Table(team_data, colWidths=[250, 250])
    t.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#DBEAFE')),
        ('TEXTCOLOR', (0,0), (-1,0), colors.HexColor('#1E3A8A')),
        ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
        ('BOTTOMPADDING', (0,0), (-1,-1), 4),
        ('TOPPADDING', (0,0), (-1,-1), 4),
        ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#93C5FD')),
    ]))
    story.append(t)
    story.append(Spacer(1, 10))

    # Project Overview
    story.append(Paragraph("1. Project Overview & Architecture", h1_style))
    story.append(Paragraph(
        "GeneScript is a domain-specific programming language designed for bioinformatics algorithms. "
        "It features built-in <b>dna</b> and <b>rna</b> data types with biological letter validation, value-first syntax "
        "(<code>\"ATGC\" -&gt; dna g</code>), prefix sequence concatenation (<code>fuse a b</code>), "
        "reverse complement (<code>~d</code>), RNA transcription (<code>transcribe(d)</code>), and specialized loops "
        "(<code>cycle i across s</code>).", body_style
    ))
    story.append(Paragraph(
        "<b>Phase 1 (Interpreter)</b>: Implemented using FLEX and Bison only, without intermediate bytecode. "
        "The parser constructs an Abstract Syntax Tree (AST), performs biological semantics validation (ensuring valid bases "
        "and presence of <code>gene nucleus()</code>), and directly evaluates the program in memory.", body_style
    ))

    # Project Structure
    story.append(Paragraph("2. Project Files and Descriptions", h1_style))
    files_data = [
        ["File / Path", "Description"],
        ["include/ast.h", "AST node definitions (Genes, Statements, Expressions, Values)."],
        ["include/symbol_table.h", "Scoped symbol table managing variable and loop-counter bindings."],
        ["include/semantics.h", "Validates biological alphabets (A,C,G,T / A,C,G,U) & types."],
        ["include/interpreter.h", "In-memory AST Tree-Walk interpreter runtime."],
        ["src/lexer.l", "Flex scanner recognizing keywords, literals, comments (^^), ops."],
        ["src/parser.y", "Bison LALR(1) grammar for value-first syntax & control flows."],
        ["src/semantics.cpp", "Biological error formatting and type inference rules."],
        ["src/interpreter.cpp", "Execution engine for biological functions (~, transcribe, fuse, etc.)."],
        ["src/main.cpp", "Driver program coordinating parsing, validation, and execution."],
        ["tests/sample1_filter.gene", "Sample 1: Filter G/C bases, reverse complement, transcription."],
        ["tests/sample2_mergesort.gene", "Sample 2: Recursive merge sort on DNA sequences."],
        ["tests/error.gene", "Sample 3: Biological error reporting for invalid base 'X'."],
        ["Makefile", "Linux build script supporting make, make clean, and make test."]
    ]
    ft = Table(files_data, colWidths=[150, 350])
    ft.setStyle(TableStyle([
        ('BACKGROUND', (0,0), (-1,0), colors.HexColor('#F3F4F6')),
        ('FONTNAME', (0,0), (-1,0), 'Helvetica-Bold'),
        ('BOTTOMPADDING', (0,0), (-1,-1), 3),
        ('TOPPADDING', (0,0), (-1,-1), 3),
        ('FONTSIZE', (0,0), (-1,-1), 8.5),
        ('GRID', (0,0), (-1,-1), 0.5, colors.HexColor('#D1D5DB')),
    ]))
    story.append(ft)
    story.append(Spacer(1, 10))

    # Dependencies & Installation
    story.append(Paragraph("3. Dependencies and Installation (Linux)", h1_style))
    story.append(Paragraph(
        "On Ubuntu / Debian Linux, install all required dependencies with:", body_style
    ))
    story.append(Preformatted("sudo apt update && sudo apt install -y build-essential flex bison clang llvm make", code_style))

    # Compilation & Running
    story.append(Paragraph("4. How to Compile and Run", h1_style))
    story.append(Paragraph("To compile the project using GNU Make:", body_style))
    story.append(Preformatted("make", code_style))

    story.append(Paragraph("To run GeneScript programs with the interpreter:", body_style))
    story.append(Preformatted("./genec tests/sample1_filter.gene\n./genec tests/sample2_mergesort.gene\n./genec tests/error.gene", code_style))

    story.append(Paragraph("To run the full automated verification test suite:", body_style))
    story.append(Preformatted("make test", code_style))

    # Examples & Verification
    story.append(Paragraph("5. Test Results & Verification", h1_style))
    story.append(Paragraph(
        "All test outputs match the proposal specification 100%:<br/>"
        "• <b>Sample 1 Output:</b><br/><code>GGCCG<br/>TACAATGGCCAT<br/>AUGGCCAUUGUA</code><br/>"
        "• <b>Sample 2 Output:</b><br/><code>AAACGTT</code><br/>"
        "• <b>Sample 3 (Biological Error Output):</b><br/>"
        "<code>tests/error.gene:1: invalid base 'X' in DNA literal.<br/>"
        "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;DNA allows only A, C, G, T.<br/>"
        "Compilation stopped: 1 error.</code>", body_style
    ))

    doc.build(story)
    print("README.pdf generated successfully.")

if __name__ == "__main__":
    build_pdf()

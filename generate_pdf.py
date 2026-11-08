"""Builds README.pdf from README.md, so the two always say the same thing.

    python3 generate_pdf.py            (or: make pdf)

Needs reportlab (pip install reportlab).  Supports the Markdown used in
README.md: # / ## / ### headings, paragraphs, - bullet lists, tables,
``` code blocks, --- rules, `inline code` and **bold**.
"""

import re
import sys

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.platypus import (Paragraph, Preformatted, SimpleDocTemplate,
                                Spacer, Table, TableStyle, HRFlowable)

SOURCE = "README.md"
TARGET = "README.pdf"
MARGIN = 42
CODE_FONT_SIZE = 7.6

BLUE_DARK = colors.HexColor("#1E3A8A")
BLUE = colors.HexColor("#1E40AF")
TEXT = colors.HexColor("#1F2937")
GRID = colors.HexColor("#CBD5E1")
HEADER_BG = colors.HexColor("#DBEAFE")
CODE_BG = colors.HexColor("#F3F4F6")

base = getSampleStyleSheet()
STYLES = {
    "title": ParagraphStyle("title", parent=base["Heading1"], fontSize=19, leading=23,
                            textColor=BLUE_DARK, spaceAfter=4),
    "h2": ParagraphStyle("h2", parent=base["Heading2"], fontSize=13.5, leading=17,
                         textColor=BLUE, spaceBefore=12, spaceAfter=5),
    "h3": ParagraphStyle("h3", parent=base["Heading3"], fontSize=11, leading=14,
                         textColor=BLUE_DARK, spaceBefore=8, spaceAfter=3),
    "body": ParagraphStyle("body", parent=base["Normal"], fontSize=9.6, leading=13.2,
                           textColor=TEXT, spaceAfter=5, alignment=TA_LEFT),
    "body_indent": ParagraphStyle("body_indent", parent=base["Normal"], fontSize=9.6,
                                  leading=13.2, textColor=TEXT, leftIndent=14, spaceAfter=5),
    "bullet": ParagraphStyle("bullet", parent=base["Normal"], fontSize=9.6, leading=13.2,
                             textColor=TEXT, leftIndent=14, bulletIndent=4, spaceAfter=2.5),
    "bullet2": ParagraphStyle("bullet2", parent=base["Normal"], fontSize=9.4, leading=12.8,
                              textColor=TEXT, leftIndent=28, bulletIndent=18, spaceAfter=2),
    "cell": ParagraphStyle("cell", parent=base["Normal"], fontSize=8.6, leading=11, textColor=TEXT),
    "cell_head": ParagraphStyle("cell_head", parent=base["Normal"], fontSize=8.6, leading=11,
                                textColor=BLUE_DARK, fontName="Helvetica-Bold"),
    "code": ParagraphStyle("code", fontName="Courier", fontSize=CODE_FONT_SIZE,
                           leading=CODE_FONT_SIZE * 1.3, textColor=colors.HexColor("#111827"),
                           backColor=CODE_BG, borderPadding=(4, 5, 4, 5),
                           spaceBefore=5, spaceAfter=9),
}


def escape(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def inline(text):
    """Markdown inline markup -> reportlab paragraph markup."""
    # Set code spans aside first, so **bold** may contain `code`.
    codes = []

    def stash(m):
        codes.append(m.group(1))
        return "\x00%d\x00" % (len(codes) - 1)

    s = escape(re.sub(r"`([^`]*)`", stash, text))
    s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
    return re.sub("\x00(\\d+)\x00",
                  lambda m: '<font face="Courier" color="#9F1239">%s</font>'
                  % escape(codes[int(m.group(1))]), s)


def split_row(line):
    cells = line.strip().strip("|").split("|")
    return [c.strip() for c in cells]


def make_table(rows, usable_width):
    header, body = rows[0], rows[1:]
    has_header = any(cell for cell in header)
    ncols = len(header)
    for r in body:
        if len(r) != ncols:
            raise ValueError("table row has %d cells, expected %d: %r" % (len(r), ncols, r))

    # Column widths proportional to content length, with a sensible minimum.
    all_rows = ([header] if has_header else []) + body
    lengths = [max(len(r[c]) for r in all_rows) for c in range(ncols)]
    lengths = [min(max(n, 8), 70) for n in lengths]
    total = float(sum(lengths))
    widths = [usable_width * n / total for n in lengths]

    data = []
    if has_header:
        data.append([Paragraph(inline(c), STYLES["cell_head"]) for c in header])
    for r in body:
        data.append([Paragraph(inline(c), STYLES["cell"]) for c in r])
    table = Table(data, colWidths=widths, repeatRows=1 if has_header else 0, hAlign="LEFT")
    style = [
        ("GRID", (0, 0), (-1, -1), 0.5, GRID),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("TOPPADDING", (0, 0), (-1, -1), 3),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
    ]
    if has_header:
        style.append(("BACKGROUND", (0, 0), (-1, 0), HEADER_BG))
    table.setStyle(TableStyle(style))
    return table


def check_code_width(lines, usable_width):
    # Courier glyphs are 0.6 em wide; reportlab does not wrap preformatted text.
    max_chars = int((usable_width - 10) / (CODE_FONT_SIZE * 0.6))
    for line in lines:
        if len(line) > max_chars:
            raise ValueError("code line longer than %d characters would be cut off:\n%s"
                             % (max_chars, line))


def convert(markdown, usable_width):
    story = []
    lines = markdown.splitlines()
    i = 0
    paragraph = []

    paragraph_indented = [False]  # first line indented: continues a list item

    def flush_paragraph():
        if paragraph:
            style = STYLES["body_indent" if paragraph_indented[0] else "body"]
            story.append(Paragraph(inline(" ".join(paragraph)), style))
            paragraph.clear()

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped.startswith("```"):
            flush_paragraph()
            i += 1
            code = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code.append(lines[i].rstrip())
                i += 1
            if i == len(lines):
                raise ValueError("unterminated code block")
            check_code_width(code, usable_width)
            story.append(Preformatted("\n".join(code), STYLES["code"]))
            i += 1
            continue

        if not stripped:
            flush_paragraph()
            i += 1
            continue

        if stripped == "---":
            flush_paragraph()
            story.append(HRFlowable(width="100%", thickness=0.6, color=GRID,
                                    spaceBefore=6, spaceAfter=6))
            i += 1
            continue

        heading = re.match(r"^(#{1,3})\s+(.*)$", stripped)
        if heading:
            flush_paragraph()
            level = len(heading.group(1))
            style = {1: "title", 2: "h2", 3: "h3"}[level]
            story.append(Paragraph(inline(heading.group(2)), STYLES[style]))
            i += 1
            continue

        if stripped.startswith("|"):
            flush_paragraph()
            rows = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                row = split_row(lines[i])
                if not all(re.fullmatch(r":?-+:?", c) for c in row):  # skip |---|---|
                    rows.append(row)
                i += 1
            story.append(Spacer(1, 3))
            story.append(make_table(rows, usable_width))
            story.append(Spacer(1, 7))
            continue

        bullet = re.match(r"^(\s*)- (.*)$", line)
        if bullet:
            flush_paragraph()
            nested = len(bullet.group(1)) >= 2
            text = [bullet.group(2).strip()]
            i += 1
            # Continuation lines are indented further than the bullet itself.
            while (i < len(lines) and lines[i].strip() and
                   not re.match(r"^\s*- ", lines[i]) and
                   len(lines[i]) - len(lines[i].lstrip()) > len(bullet.group(1))):
                text.append(lines[i].strip())
                i += 1
            style = STYLES["bullet2" if nested else "bullet"]
            story.append(Paragraph(inline(" ".join(text)), style,
                                   bulletText="-" if nested else "•"))
            continue

        numbered = re.match(r"^(\d+)\. (.*)$", stripped)
        if numbered:
            flush_paragraph()
            text = [numbered.group(2).strip()]
            i += 1
            while (i < len(lines) and lines[i].strip() and lines[i].startswith("   ") and
                   not re.match(r"^\s*- ", lines[i])):
                text.append(lines[i].strip())
                i += 1
            story.append(Paragraph(inline(" ".join(text)), STYLES["bullet"],
                                   bulletText=numbered.group(1) + "."))
            continue

        if not paragraph:
            paragraph_indented[0] = line.startswith("   ")
        paragraph.append(stripped)
        i += 1

    flush_paragraph()
    return story


def page_footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Helvetica", 8)
    canvas.setFillColor(colors.HexColor("#6B7280"))
    canvas.drawString(MARGIN, 22, "GeneScript - Compiler Construction (BCS-7G)")
    canvas.drawRightString(A4[0] - MARGIN, 22, "Page %d" % doc.page)
    canvas.restoreState()


def build_pdf(source=SOURCE, target=TARGET):
    with open(source, encoding="utf-8") as f:
        markdown = f.read()
    non_latin1 = sorted({c for c in markdown if ord(c) > 255})
    if non_latin1:
        raise ValueError("README.md uses characters the PDF fonts cannot show: %r" % non_latin1)

    doc = SimpleDocTemplate(target, pagesize=A4, leftMargin=MARGIN, rightMargin=MARGIN,
                            topMargin=MARGIN, bottomMargin=MARGIN + 6,
                            title="GeneScript - README",
                            author="Eman Aamir, Haleema Sadia, Ahsan Naveed")
    story = convert(markdown, A4[0] - 2 * MARGIN)
    doc.build(story, onFirstPage=page_footer, onLaterPages=page_footer)
    print("%s generated from %s." % (target, source))


if __name__ == "__main__":
    try:
        build_pdf()
    except ValueError as e:
        sys.exit("generate_pdf.py: %s" % e)

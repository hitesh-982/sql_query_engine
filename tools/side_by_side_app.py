#!/usr/bin/env python3
"""
side_by_side_app.py - Advanced Interactive Web Interface for SQL Query Engine
A complete SQL Query Engine studio with a Supabase/DataGrip-inspired dark aesthetic,
syntax-highlighted SQL editor, real-time AST S-expression inspector, physical query
planner visualizer, SIMD CSV scanner throughput benchmarks, and SQLite oracle comparison.
"""

import csv
import glob
import http.server
import io
import json
import math
import os
import re
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.parse

# Ensure local tools directory is importable
script_dir = os.path.dirname(os.path.abspath(__file__))
if script_dir not in sys.path:
    sys.path.insert(0, script_dir)

from compare import load_csv_into_sqlite, cells_match, rows_match, make_row_sort_key

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Fast-SQL - High-Performance Query Engine Studio</title>
  <!-- Tailwind CSS CDN -->
  <script src="https://www.gstatic.com/antigravity/web/dev/tailwindcss.min.js"></script>
  <!-- CodeMirror CSS -->
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.css">
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/hint/show-hint.min.css">
  <!-- CodeMirror JS & SQL Mode & Addons -->
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/sql/sql.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/hint/show-hint.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/hint/sql-hint.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/edit/matchbrackets.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/edit/closebrackets.min.js"></script>

  <style>
    body {
      background-color: #f8fafc;
      background-image: radial-gradient(#cbd5e1 1.1px, transparent 1.1px);
      background-size: 24px 24px;
      color: #0f172a;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    }
    .code-font {
      font-family: ui-monospace, SFMono-Regular, "JetBrains Mono", Menlo, Monaco, Consolas, monospace;
    }
    /* Fast-SQL Custom CodeMirror Theme */
    .cm-s-sqlai.CodeMirror {
      background: #0b0f19 !important;
      color: #e2e8f0 !important;
      height: 380px;
      font-family: ui-monospace, SFMono-Regular, "JetBrains Mono", Menlo, Monaco, Consolas, monospace;
      font-size: 13.5px;
      line-height: 1.6;
      border-radius: 1rem;
      padding: 12px 6px;
      border: 1px solid #1e293b;
    }
    .cm-s-sqlai .CodeMirror-gutters {
      background: #0b0f19 !important;
      border-right: 1px solid #1e293b;
    }
    .cm-s-sqlai .CodeMirror-linenumber {
      color: #334155 !important;
    }
    .cm-s-sqlai .cm-keyword {
      color: #f43f5e !important; /* Coral Pink / Rose */
      font-weight: 700;
    }
    .cm-s-sqlai .cm-variable,
    .cm-s-sqlai .cm-variable-2,
    .cm-s-sqlai .cm-def {
      color: #38bdf8 !important; /* Ice Cyan / Sky Blue */
    }
    .cm-s-sqlai .cm-string {
      color: #86efac !important; /* Mint Green */
    }
    .cm-s-sqlai .cm-number {
      color: #fde047 !important; /* Soft Amber */
    }
    .cm-s-sqlai .cm-operator,
    .cm-s-sqlai .cm-punctuation {
      color: #94a3b8 !important; /* Slate gray */
    }
    .cm-s-sqlai .CodeMirror-cursor {
      border-left: 2px solid #38bdf8 !important;
    }
    ::-webkit-scrollbar { width: 6px; height: 6px; }
    ::-webkit-scrollbar-track { background: #f1f5f9; }
    ::-webkit-scrollbar-thumb { background: #cbd5e1; border-radius: 4px; }
    ::-webkit-scrollbar-thumb:hover { background: #94a3b8; }
    .badge-null {
      font-size: 10px;
      font-weight: 600;
      padding: 1px 5px;
      border-radius: 4px;
      background: rgba(148, 163, 184, 0.15);
      color: #64748b;
      border: 1px solid rgba(148, 163, 184, 0.3);
      font-style: italic;
    }
  </style>
</head>
<body class="min-h-screen flex antialiased">

  <!-- Left Sidebar (Fast-SQL Navigation) -->
  <aside class="w-56 shrink-0 bg-white border-r border-slate-200 flex flex-col justify-between p-4 hidden md:flex select-none">
    <div>
      <!-- Brand Logo -->
      <div class="flex items-center gap-2.5 px-2 py-3 mb-4">
        <div class="w-7 h-7 rounded-lg bg-slate-900 text-white flex items-center justify-center font-bold text-sm shadow-sm">
          <svg class="w-4 h-4 text-sky-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M13 10V3L4 14h7v7l9-11h-7z"/></svg>
        </div>
        <span class="font-extrabold text-base tracking-tight text-slate-900">Fast<span class="text-sky-600 font-black">-SQL</span></span>
      </div>

      <!-- Menu Section -->
      <div class="text-[11px] font-semibold text-slate-400 uppercase tracking-wider px-2 mb-2">Menu</div>
      <nav class="space-y-0.5 text-xs font-medium text-slate-600">
        <a href="#" class="flex items-center justify-between px-2.5 py-2 rounded-lg bg-slate-100 text-slate-900 font-semibold">
          <span class="flex items-center gap-2.5">
            <svg class="w-4 h-4 text-slate-700" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2V6zM14 6a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2V6zM4 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2H6a2 2 0 01-2-2v-2zM14 16a2 2 0 012-2h2a2 2 0 012 2v2a2 2 0 01-2 2h-2a2 2 0 01-2-2v-2z"/></svg>
            Generators
          </span>
          <svg class="w-3 h-3 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5l7 7-7 7"/></svg>
        </a>
        <a href="#" class="flex items-center justify-between px-2.5 py-2 rounded-lg hover:bg-slate-50 text-slate-600 hover:text-slate-900 transition">
          <span class="flex items-center gap-2.5">
            <svg class="w-4 h-4 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 7v10c0 2 1.5 3 3.5 3h9c2 0 3.5-1 3.5-3V7M4 7c0-2 1.5-3 3.5-3h9c2 0 3.5 1 3.5 3M4 7h16m-16 5h16"/></svg>
            Datasources
          </span>
          <svg class="w-3 h-3 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5l7 7-7 7"/></svg>
        </a>
        <a href="#" class="flex items-center gap-2.5 px-2.5 py-2 rounded-lg hover:bg-slate-50 text-slate-600 hover:text-slate-900 transition">
          <svg class="w-4 h-4 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z"/></svg>
          Queries
        </a>
        <a href="#" class="flex items-center gap-2.5 px-2.5 py-2 rounded-lg hover:bg-slate-50 text-slate-600 hover:text-slate-900 transition">
          <svg class="w-4 h-4 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/></svg>
          Settings
        </a>
        <a href="#" class="flex items-center gap-2.5 px-2.5 py-2 rounded-lg hover:bg-slate-50 text-slate-600 hover:text-slate-900 transition">
          <svg class="w-4 h-4 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 8l7.89 5.26a2 2 0 002.22 0L21 8M5 19h14a2 2 0 002-2V7a2 2 0 00-2-2H5a2 2 0 00-2 2v10a2 2 0 002 2z"/></svg>
          Contact
        </a>
        <a href="#" class="flex items-center gap-2.5 px-2.5 py-2 rounded-lg hover:bg-slate-50 text-slate-600 hover:text-slate-900 transition">
          <svg class="w-4 h-4 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 9l3 3-3 3m5 0h3M5 20h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>
          Public API
        </a>
      </nav>
    </div>

    <!-- Bottom Sidebar -->
    <div class="border-t border-slate-100 pt-3 text-xs text-slate-400">
      <div class="flex items-center gap-2 text-slate-600 font-semibold mb-1">
        <span class="w-2 h-2 rounded-full bg-emerald-500"></span> Engine Online
      </div>
      <div class="text-[11px]">C++17 SIMD Vectorized</div>
    </div>
  </aside>

  <!-- Main Content Area -->
  <div class="flex-1 flex flex-col min-w-0 overflow-y-auto">

    <!-- Top Breadcrumb Header -->
    <header class="border-b border-slate-200/90 bg-white/80 backdrop-blur px-6 py-3 flex items-center justify-between gap-3 sticky top-0 z-20">
      <div class="flex items-center gap-2.5 text-xs text-slate-600">
        <button class="p-1 hover:bg-slate-100 rounded text-slate-500">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 6h16M4 12h16M4 18h7"/></svg>
        </button>
        <span class="text-slate-300">|</span>
        <span class="text-slate-500 font-medium">SQL and NoSQL generators</span>
        <svg class="w-3 h-3 text-slate-400 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5l7 7-7 7"/></svg>
        <span class="text-slate-900 font-semibold">Fast-SQL (Query Engine & Oracle Benchmark)</span>
        <span class="w-4 h-4 rounded-full bg-slate-100 text-slate-500 flex items-center justify-center text-[10px] font-bold">i</span>
      </div>

      <div class="flex items-center gap-2 text-xs">
        <span class="px-2.5 py-1 rounded-full bg-slate-100 text-slate-700 font-medium border border-slate-200">
          Fast-SQL vs SQLite 3.54 Oracle
        </span>
      </div>
    </header>

    <!-- Content Workspace -->
    <main class="flex-1 p-4 md:p-6 flex flex-col lg:flex-row gap-6 relative items-start">

      <!-- Left / Center Column (Instructions & Datasource) -->
      <div class="w-full lg:w-[420px] shrink-0 flex flex-col gap-5">

        <!-- Card 1: Instructions Box -->
        <div class="bg-white border border-slate-200/90 rounded-2xl p-4 shadow-sm flex flex-col gap-3">
          <div class="flex justify-between items-center">
            <label class="text-xs font-bold text-slate-800 tracking-tight">Instructions</label>
            <select id="presetSelect" onchange="applyPreset()" class="text-[11px] bg-slate-50 border border-slate-200 text-slate-700 rounded-lg px-2 py-1 outline-none">
              <option value="S4">Preset S4: String Group By & Agg</option>
              <option value="S8">Preset S8: DISTINCT city (2.6x speedup)</option>
              <option value="S6">Preset S6: Top-10 ORDER BY amount DESC</option>
              <option value="J1">Preset J1: 2-Table INNER JOIN</option>
              <option value="J6">Preset J6: FULL OUTER JOIN (2.4x speedup)</option>
              <option value="S1">Preset S1: Filter & Scan (WHERE amount > 500)</option>
            </select>
          </div>

          <textarea id="instructionsText" rows="4" class="w-full bg-slate-50/70 border border-slate-200 rounded-xl p-3 text-xs text-slate-800 focus:bg-white focus:border-slate-400 focus:outline-none transition leading-relaxed resize-none">Select city, count of transactions, and total sum of amount from table t, grouped by city, ordered by city, limited to top 10 records.</textarea>

          <!-- Buttons Row Below Textarea -->
          <div class="flex flex-wrap items-center gap-2 pt-1">
            <button onclick="runQuery()" id="runBtn" class="bg-slate-900 hover:bg-black text-white px-4 py-2 rounded-xl text-xs font-semibold flex items-center gap-1.5 shadow-sm transition">
              <span id="runBtnIcon">
                <svg class="w-3.5 h-3.5 fill-current" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
              </span>
              <span id="runBtnText">Generate query</span>
            </button>
            <button onclick="formatEditorSql()" class="bg-white border border-slate-200 hover:bg-slate-50 text-slate-700 px-3.5 py-2 rounded-xl text-xs font-medium shadow-sm flex items-center gap-1.5 transition">
              <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z"/><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 12a3 3 0 11-6 0 3 3 0 016 0z"/></svg>
              Settings
            </button>
            <div class="relative">
              <select id="datasetSelect" onchange="runQuery()" class="appearance-none bg-white border border-slate-200 hover:bg-slate-50 text-slate-700 pl-3 pr-7 py-2 rounded-xl text-xs font-medium shadow-sm outline-none transition cursor-pointer">
                <option value="100k">100k rows (data_100k.csv)</option>
                <option value="small">10 rows (data_small.csv)</option>
                <option value="5m">5M rows (data_5m.csv)</option>
              </select>
              <svg class="w-3 h-3 text-slate-400 absolute right-2.5 top-2.5 pointer-events-none" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
            </div>
          </div>
        </div>

        <!-- Card 2: Fast-SQL Database Explorer -->
        <div class="bg-white border border-slate-200/90 rounded-2xl p-4 shadow-sm flex flex-col gap-3">
          <div class="flex justify-between items-start">
            <div>
              <h2 class="text-sm font-bold text-slate-900 tracking-tight">Fast-SQL Database (CSV Engine)</h2>
              <p class="text-[11px] text-slate-500 mt-0.5">Vectorized in-memory CSV scanner & tables</p>
            </div>
            <!-- Pill Toggle: Schema / Rules -->
            <div class="flex bg-slate-100 p-0.5 rounded-lg text-xs font-semibold">
              <button class="bg-slate-900 text-white px-2.5 py-1 rounded-md text-[11px] shadow-sm">Schema</button>
              <button onclick="switchView('explain')" class="text-slate-600 hover:text-slate-900 px-2.5 py-1 rounded-md text-[11px]">Rules</button>
            </div>
          </div>

          <!-- Search Input -->
          <div class="relative">
            <input type="text" id="datasourceSearch" oninput="filterDatasource(this.value)" placeholder="Search datasource..." class="w-full bg-slate-50 border border-slate-200 rounded-xl px-3 py-2 text-xs text-slate-800 placeholder-slate-400 focus:bg-white focus:border-slate-400 focus:outline-none transition">
          </div>

          <!-- Accordion Tables List -->
          <div id="datasourceList" class="space-y-1.5 text-xs text-slate-700 mt-1 max-h-72 overflow-y-auto">
            <!-- Table t -->
            <div class="border border-slate-100 rounded-xl overflow-hidden">
              <button onclick="toggleAccordion('acc_t')" class="w-full flex justify-between items-center p-2.5 bg-slate-50/70 hover:bg-slate-100 text-left font-medium text-slate-800 transition">
                <span class="flex items-center gap-2">
                  <svg class="w-3.5 h-3.5 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 7v10c0 2 1.5 3 3.5 3h9c2 0 3.5-1 3.5-3V7M4 7c0-2 1.5-3 3.5-3h9c2 0 3.5 1 3.5 3M4 7h16m-16 5h16"/></svg>
                  <span class="font-bold text-sky-600">t</span> <span class="text-[10px] text-slate-400 font-mono">(100,000 rows)</span>
                </span>
                <svg id="icon_acc_t" class="w-3 h-3 text-slate-400 transition-transform duration-200" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
              </button>
              <div id="acc_t" class="p-2.5 bg-white border-t border-slate-100 text-[11px] font-mono space-y-1 text-slate-600">
                <div onclick="insertColumn('city')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>city</span> <span class="text-emerald-500">STRING</span></div>
                <div onclick="insertColumn('amount')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>amount</span> <span class="text-indigo-500">FLOAT</span></div>
                <div onclick="insertColumn('id')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>id</span> <span class="text-sky-500">INT</span></div>
                <div onclick="insertColumn('is_active')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>is_active</span> <span class="text-amber-500">BOOL</span></div>
              </div>
            </div>

            <!-- Table orders -->
            <div class="border border-slate-100 rounded-xl overflow-hidden">
              <button onclick="toggleAccordion('acc_orders')" class="w-full flex justify-between items-center p-2.5 bg-slate-50/70 hover:bg-slate-100 text-left font-medium text-slate-800 transition">
                <span class="flex items-center gap-2">
                  <svg class="w-3.5 h-3.5 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 7v10c0 2 1.5 3 3.5 3h9c2 0 3.5-1 3.5-3V7M4 7c0-2 1.5-3 3.5-3h9c2 0 3.5 1 3.5 3M4 7h16m-16 5h16"/></svg>
                  <span class="font-bold text-indigo-600">orders</span> <span class="text-[10px] text-slate-400 font-mono">(2,000 rows)</span>
                </span>
                <svg id="icon_acc_orders" class="w-3 h-3 text-slate-400 transition-transform duration-200" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
              </button>
              <div id="acc_orders" class="hidden p-2.5 bg-white border-t border-slate-100 text-[11px] font-mono space-y-1 text-slate-600">
                <div onclick="insertColumn('orders.id')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>id</span> <span class="text-sky-500">INT</span></div>
                <div onclick="insertColumn('orders.cust_id')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>cust_id</span> <span class="text-sky-500">INT</span></div>
                <div onclick="insertColumn('orders.total')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>total</span> <span class="text-indigo-500">FLOAT</span></div>
              </div>
            </div>

            <!-- Table customers -->
            <div class="border border-slate-100 rounded-xl overflow-hidden">
              <button onclick="toggleAccordion('acc_customers')" class="w-full flex justify-between items-center p-2.5 bg-slate-50/70 hover:bg-slate-100 text-left font-medium text-slate-800 transition">
                <span class="flex items-center gap-2">
                  <svg class="w-3.5 h-3.5 text-slate-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 7v10c0 2 1.5 3 3.5 3h9c2 0 3.5-1 3.5-3V7M4 7c0-2 1.5-3 3.5-3h9c2 0 3.5 1 3.5 3M4 7h16m-16 5h16"/></svg>
                  <span class="font-bold text-emerald-600">customers</span> <span class="text-[10px] text-slate-400 font-mono">(500 rows)</span>
                </span>
                <svg id="icon_acc_customers" class="w-3 h-3 text-slate-400 transition-transform duration-200" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M19 9l-7 7-7-7"/></svg>
              </button>
              <div id="acc_customers" class="hidden p-2.5 bg-white border-t border-slate-100 text-[11px] font-mono space-y-1 text-slate-600">
                <div onclick="insertColumn('customers.id')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>id</span> <span class="text-sky-500">INT</span></div>
                <div onclick="insertColumn('customers.name')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>name</span> <span class="text-emerald-500">STRING</span></div>
                <div onclick="insertColumn('customers.city')" class="cursor-pointer hover:text-sky-600 flex justify-between py-0.5"><span>city</span> <span class="text-emerald-500">STRING</span></div>
              </div>
            </div>
          </div>
        </div>

      </div>

      <!-- Floating Center Play Button -->
      <div class="hidden lg:flex items-center justify-center self-center -mx-4 z-10">
        <button onclick="runQuery()" class="w-12 h-12 rounded-full bg-white shadow-xl border border-slate-200 flex items-center justify-center text-slate-900 hover:bg-slate-50 hover:scale-105 transition-all focus:outline-none" title="Run Query (Ctrl + Enter)">
          <svg class="w-5 h-5 ml-0.5 text-slate-900 fill-current" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
        </button>
      </div>

      <!-- Right Panel: Proper Arrangement (Pill Actions + Dark SQL Editor + Telemetry & Results) -->
      <div class="flex-1 min-w-0 flex flex-col gap-4 w-full">

        <!-- Top Right Pill Actions Bar -->
        <div class="flex flex-wrap items-center justify-between gap-2">
          <div class="flex flex-wrap items-center gap-1.5">
            <button id="pillRun" onclick="runQuery()" class="bg-slate-900 text-white font-semibold px-3.5 py-1.5 rounded-full text-xs shadow-sm hover:bg-black transition flex items-center gap-1.5">
              <svg class="w-3 h-3 fill-current" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>
              <span>Run query</span>
            </button>
            <button id="pillEdit" onclick="formatEditorSql()" class="bg-white border border-slate-200 text-slate-700 hover:bg-slate-50 px-3 py-1.5 rounded-full text-xs font-medium shadow-sm transition">
              Edit query
            </button>
            <button id="tabAst" onclick="switchView('ast')" class="bg-white border border-slate-200 text-slate-700 hover:bg-slate-50 px-3 py-1.5 rounded-full text-xs font-medium shadow-sm transition flex items-center gap-1.5">
              <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M7 7h10M7 7a2 2 0 100-4 2 2 0 000 4zm10 0a2 2 0 100-4 2 2 0 000 4zM7 17a2 2 0 100-4 2 2 0 000 4zm10 0a2 2 0 100-4 2 2 0 000 4zM7 7v6a2 2 0 002 2h6"/></svg>
              <span>AST Expression</span>
            </button>
            <button id="tabExplain" onclick="switchView('explain')" class="bg-white border border-slate-200 text-slate-700 hover:bg-slate-50 px-3 py-1.5 rounded-full text-xs font-medium shadow-sm transition flex items-center gap-1.5">
              <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2m-3 7h3m-3 4h3m-6-4h.01M9 16h.01"/></svg>
              <span>Operator Plan</span>
            </button>
            <button id="tabDiff" onclick="switchView('diff')" class="bg-white border border-slate-200 text-slate-700 hover:bg-slate-50 px-3 py-1.5 rounded-full text-xs font-medium shadow-sm transition flex items-center gap-1.5">
              <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 7h12m0 0l-4-4m4 4l-4 4m0 6H4m0 0l4 4m-4-4l4-4"/></svg>
              <span>History & Diff</span>
            </button>
            <button id="tabSideBySide" onclick="switchView('sideBySide')" class="bg-white border border-slate-200 text-slate-700 hover:bg-slate-50 px-3 py-1.5 rounded-full text-xs font-medium shadow-sm transition flex items-center gap-1.5">
              <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M3 10h18M3 14h18m-9-4v8m-7 4h14a2 2 0 002-2V6a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>
              <span>Solution Tables</span>
            </button>
          </div>

          <div class="flex items-center gap-2 text-xs text-slate-500">
            <span id="runStatusText" class="font-mono text-[11px] text-sky-600 font-semibold"></span>
            <kbd class="hidden sm:inline-block bg-white text-slate-500 px-2 py-0.5 rounded border border-slate-200 text-[10px] font-mono shadow-sm">Ctrl + Enter</kbd>
          </div>
        </div>

        <!-- Dark SQL Code Editor Card -->
        <div class="bg-[#0b0f19] rounded-2xl p-5 md:p-6 shadow-2xl border border-slate-800 text-slate-100 relative">
          <div class="flex justify-between items-center mb-3 text-xs text-slate-400 font-mono">
            <span class="flex items-center gap-2 text-slate-300 font-semibold">
              <span class="w-2.5 h-2.5 rounded-full bg-rose-500 inline-block"></span>
              Fast-SQL Query Editor
            </span>
            <div class="flex items-center gap-2">
              <button onclick="copySQL()" class="hover:text-white transition">Copy</button>
              <span class="text-slate-600">•</span>
              <button onclick="clearEditor()" class="hover:text-white transition">Clear</button>
            </div>
          </div>

          <!-- CodeMirror editor textarea -->
          <textarea id="sqlInput" class="w-full bg-transparent text-slate-100 font-mono text-sm leading-relaxed outline-none">SELECT city, COUNT(*), SUM(amount) FROM t GROUP BY city ORDER BY city LIMIT 10</textarea>
        </div>

        <!-- Performance Benchmark KPI Cards (Engine vs SQLite) -->
        <div id="statsBar" class="grid grid-cols-2 sm:grid-cols-4 gap-3">
          <!-- Engine Latency -->
          <div class="bg-white border border-slate-200/90 rounded-2xl p-3.5 shadow-sm relative overflow-hidden">
            <div class="flex justify-between items-center text-[10px] text-slate-500 uppercase tracking-wider font-bold">
              <span>Engine Latency</span>
              <span class="text-sky-600 font-mono">C++17 SIMD</span>
            </div>
            <div id="engTime" class="text-2xl font-extrabold text-slate-900 mt-1 code-font">14.92 ms</div>
            <div class="flex justify-between items-center mt-1 text-[11px] text-slate-500">
              <span id="engRows">10 rows</span>
              <span class="text-emerald-600 font-semibold">0 disk spills</span>
            </div>
            <div class="absolute bottom-0 left-0 right-0 h-1 bg-sky-500"></div>
          </div>

          <!-- SQLite Latency -->
          <div class="bg-white border border-slate-200/90 rounded-2xl p-3.5 shadow-sm relative overflow-hidden">
            <div class="flex justify-between items-center text-[10px] text-slate-500 uppercase tracking-wider font-bold">
              <span>SQLite Oracle</span>
              <span class="text-indigo-600 font-mono">In-memory</span>
            </div>
            <div id="sqTime" class="text-2xl font-extrabold text-slate-900 mt-1 code-font">28.38 ms</div>
            <div class="flex justify-between items-center mt-1 text-[11px] text-slate-500">
              <span id="sqRows">10 rows</span>
              <span class="text-slate-400">Baseline</span>
            </div>
            <div class="absolute bottom-0 left-0 right-0 h-1 bg-indigo-500"></div>
          </div>

          <!-- Speedup Ratio -->
          <div class="bg-white border border-slate-200/90 rounded-2xl p-3.5 shadow-sm relative overflow-hidden">
            <div class="flex justify-between items-center text-[10px] text-slate-500 uppercase tracking-wider font-bold">
              <span>Speedup Factor</span>
              <svg class="w-4 h-4 text-emerald-600 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M13 7h8m0 0v8m0-8l-8 8-4-4-6 6"/></svg>
            </div>
            <div id="speedup" class="text-2xl font-extrabold text-emerald-600 mt-1 code-font">1.90x</div>
            <div class="flex justify-between items-center mt-1 text-[11px]">
              <span id="speedupLabel" class="text-emerald-600 font-bold uppercase">+90.0% FASTER</span>
            </div>
            <div class="absolute bottom-0 left-0 right-0 h-1 bg-emerald-500"></div>
          </div>

          <!-- Exact Match Verification -->
          <div class="bg-white border border-slate-200/90 rounded-2xl p-3.5 shadow-sm relative overflow-hidden">
            <div class="flex justify-between items-center text-[10px] text-slate-500 uppercase tracking-wider font-bold">
              <span>Verification</span>
              <span class="text-slate-400 font-mono">tol: 1e-6</span>
            </div>
            <div id="matchBadge" class="text-lg font-extrabold text-emerald-600 mt-1.5 flex items-center gap-1.5">
              <svg class="w-4 h-4 text-emerald-600 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M5 13l4 4L19 7"/></svg>
              <span>EXACT MATCH</span>
            </div>
            <div id="matchDesc" class="text-[11px] text-slate-500 mt-0.5 truncate">All rows match SQLite oracle</div>
            <div class="absolute bottom-0 left-0 right-0 h-1 bg-emerald-500"></div>
          </div>
        </div>

        <!-- Comparative Latency Bar -->
        <div class="bg-white border border-slate-200/90 rounded-xl p-3 shadow-sm flex flex-col gap-1.5">
          <div class="flex justify-between text-xs text-slate-600">
            <span class="flex items-center gap-1.5"><span class="w-2.5 h-2.5 rounded bg-sky-500 inline-block"></span> Custom Engine: <strong id="barEngVal" class="text-sky-700">14.92 ms (34%)</strong></span>
            <span class="flex items-center gap-1.5"><strong id="barSqVal" class="text-indigo-700">28.38 ms (66%)</strong> :SQLite Oracle <span class="w-2.5 h-2.5 rounded bg-indigo-500 inline-block"></span></span>
          </div>
          <div class="w-full bg-slate-100 rounded-full h-2 flex overflow-hidden p-0.5 border border-slate-200">
            <div id="barEng" class="bg-sky-500 h-full rounded-l transition-all duration-300" style="width: 34%"></div>
            <div id="barSq" class="bg-indigo-500 h-full rounded-r transition-all duration-300" style="width: 66%"></div>
          </div>
        </div>

        <!-- Dynamic Results Containers -->
        <div id="resultsContainer" class="flex-1 flex flex-col">

          <!-- Tab: Solution Tables -->
          <div id="viewSideBySide" class="grid grid-cols-1 lg:grid-cols-2 gap-4">
            <!-- Engine Solution Table -->
            <div class="bg-white border border-slate-200/90 rounded-2xl overflow-hidden shadow-sm flex flex-col">
              <div class="bg-slate-50 px-4 py-2.5 border-b border-slate-200 flex justify-between items-center text-xs">
                <span class="font-bold text-sky-700 flex items-center gap-2">
                  <span class="w-2 h-2 rounded-full bg-sky-500"></span> Engine Solution Table
                </span>
                <span id="engRowCount" class="text-slate-500 font-mono text-[11px]">10 rows</span>
              </div>
              <div class="p-3 overflow-x-auto max-h-[440px] overflow-y-auto code-font text-xs flex-1">
                <table id="engTable" class="w-full text-left border-collapse">
                  <thead class="bg-slate-50 text-slate-500 border-b border-slate-200 sticky top-0 text-[11px]">
                    <tr><th class="p-2 border-r border-slate-200 w-10 text-center text-slate-400">#</th><th class="p-2">city</th><th class="p-2">count(*)</th><th class="p-2">sum(amount)</th></tr>
                  </thead>
                  <tbody class="divide-y divide-slate-100 text-slate-700">
                    <tr class="hover:bg-slate-50"><td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono">1</td><td class="p-2"><span class="badge-null">NULL</span></td><td class="p-2">1009</td><td class="p-2">4885468.78</td></tr>
                    <tr class="hover:bg-slate-50"><td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono">2</td><td class="p-2">City_1</td><td class="p-2">13170</td><td class="p-2">62783946.60</td></tr>
                    <tr class="hover:bg-slate-50"><td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono">3</td><td class="p-2">City_10</td><td class="p-2">1366</td><td class="p-2">6325997.99</td></tr>
                  </tbody>
                </table>
              </div>
              <div id="engPagination" class="bg-slate-50 px-3.5 py-2.5 border-t border-slate-200 flex flex-wrap justify-between items-center text-xs text-slate-600 gap-2"></div>
            </div>

            <!-- SQLite Solution Table -->
            <div class="bg-white border border-slate-200/90 rounded-2xl overflow-hidden shadow-sm flex flex-col">
              <div class="bg-slate-50 px-4 py-2.5 border-b border-slate-200 flex justify-between items-center text-xs">
                <span class="font-bold text-indigo-700 flex items-center gap-2">
                  <span class="w-2 h-2 rounded-full bg-indigo-500"></span> SQLite Oracle Table
                </span>
                <span id="sqRowCount" class="text-slate-500 font-mono text-[11px]">10 rows</span>
              </div>
              <div class="p-3 overflow-x-auto max-h-[440px] overflow-y-auto code-font text-xs flex-1">
                <table id="sqTable" class="w-full text-left border-collapse">
                  <thead class="bg-slate-50 text-slate-500 border-b border-slate-200 sticky top-0 text-[11px]">
                    <tr><th class="p-2 border-r border-slate-200 w-10 text-center text-slate-400">#</th><th class="p-2">city</th><th class="p-2">COUNT(*)</th><th class="p-2">SUM(amount)</th></tr>
                  </thead>
                  <tbody class="divide-y divide-slate-100 text-slate-700">
                    <tr class="hover:bg-slate-50"><td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono">1</td><td class="p-2"><span class="badge-null">NULL</span></td><td class="p-2">1009</td><td class="p-2">4885468.78</td></tr>
                    <tr class="hover:bg-slate-50"><td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono">2</td><td class="p-2">City_1</td><td class="p-2">13170</td><td class="p-2">62783946.60</td></tr>
                    <tr class="hover:bg-slate-50"><td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono">3</td><td class="p-2">City_10</td><td class="p-2">1366</td><td class="p-2">6325997.99</td></tr>
                  </tbody>
                </table>
              </div>
              <div id="sqPagination" class="bg-slate-50 px-3.5 py-2.5 border-t border-slate-200 flex flex-wrap justify-between items-center text-xs text-slate-600 gap-2"></div>
            </div>
          </div>

          <!-- Tab: Unified Diff -->
          <div id="viewDiff" class="hidden bg-white border border-slate-200/90 rounded-2xl overflow-hidden shadow-sm flex flex-col p-4">
            <div class="flex justify-between items-center mb-3">
              <span class="text-xs font-bold text-slate-800 uppercase tracking-wider">Side-by-Side Unified Value Diff</span>
              <span id="diffSummaryBadge" class="text-xs font-mono text-emerald-600 font-semibold">100% Matching (10 rows)</span>
            </div>
            <div class="overflow-x-auto max-h-[440px] overflow-y-auto code-font text-xs">
              <table id="diffTable" class="w-full text-left border-collapse"></table>
            </div>
            <div id="diffPagination" class="bg-slate-50 px-3.5 py-2.5 border-t border-slate-200 rounded-b-xl flex flex-wrap justify-between items-center text-xs text-slate-600 gap-2 mt-3"></div>
          </div>

          <!-- Tab: AST S-Expression View -->
          <div id="viewAst" class="hidden bg-white border border-slate-200/90 rounded-2xl overflow-hidden shadow-sm flex flex-col p-5">
            <div class="flex flex-wrap justify-between items-center gap-3 mb-3">
              <div>
                <h2 class="text-xs font-bold text-emerald-700 uppercase tracking-wider flex items-center gap-2">
                  <svg class="w-4 h-4 text-emerald-600" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M7 7h10M7 7a2 2 0 100-4 2 2 0 000 4zm10 0a2 2 0 100-4 2 2 0 000 4zM7 17a2 2 0 100-4 2 2 0 000 4zm10 0a2 2 0 100-4 2 2 0 000 4zM7 7v6a2 2 0 002 2h6"/></svg>
                  <span>Abstract Syntax Tree & Canonical SQL</span>
                </h2>
                <p class="text-[11px] text-slate-500 mt-0.5">Parsed via recursive-descent SqlParser and serialized to canonical S-expression</p>
              </div>
              <div class="flex items-center gap-2">
                <button onclick="copyAstSExpr()" class="text-xs bg-slate-100 hover:bg-slate-200 text-slate-700 border border-slate-200 px-3 py-1.5 rounded-lg transition flex items-center gap-1.5 font-medium">
                  <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M8 16H6a2 2 0 01-2-2V6a2 2 0 012-2h8a2 2 0 012 2v2m-6 12h8a2 2 0 002-2v-8a2 2 0 00-2-2h-8a2 2 0 00-2 2v8a2 2 0 002 2z"/></svg>
                  <span>Copy AST</span>
                </button>
                <button onclick="copyCanonicalSql()" class="text-xs bg-slate-100 hover:bg-slate-200 text-slate-700 border border-slate-200 px-3 py-1.5 rounded-lg transition flex items-center gap-1.5 font-medium">
                  <svg class="w-3.5 h-3.5 text-slate-500" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z"/></svg>
                  <span>Copy SQL</span>
                </button>
                <button onclick="updateAstView()" class="text-xs bg-emerald-50 hover:bg-emerald-100 text-emerald-700 border border-emerald-200 px-3 py-1.5 rounded-lg transition flex items-center gap-1.5 font-semibold">
                  <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15"/></svg>
                  <span>Refresh AST</span>
                </button>
              </div>
            </div>

            <!-- AST Highlights Cards -->
            <div id="astSummaryCards" class="grid grid-cols-2 sm:grid-cols-5 gap-2.5 mb-4 text-xs font-mono"></div>

            <!-- Side-by-side: S-Expression + Canonical SQL -->
            <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
              <div class="bg-[#0b0f19] p-4 rounded-xl border border-slate-800">
                <div class="flex justify-between items-center mb-2">
                  <span class="text-xs font-bold text-slate-300 uppercase tracking-wider font-mono flex items-center gap-1.5"><span class="w-2 h-2 rounded-full bg-emerald-400"></span> AST S-Expression</span>
                  <span class="text-[10px] text-slate-500 font-mono">SelectStmt::toSExpr()</span>
                </div>
                <pre id="astOutput" class="code-font text-xs text-slate-200 overflow-x-auto leading-relaxed h-[360px] whitespace-pre bg-slate-950/80 p-3 rounded-lg border border-slate-900"></pre>
              </div>

              <div class="bg-[#0b0f19] p-4 rounded-xl border border-slate-800">
                <div class="flex justify-between items-center mb-2">
                  <span class="text-xs font-bold text-slate-300 uppercase tracking-wider font-mono flex items-center gap-1.5"><span class="w-2 h-2 rounded-full bg-sky-400"></span> Canonical Roundtrip SQL</span>
                  <span class="text-[10px] text-slate-500 font-mono">SelectStmt::toSql()</span>
                </div>
                <pre id="canonicalSqlOutput" class="code-font text-xs text-sky-300 overflow-x-auto leading-relaxed h-[360px] whitespace-pre bg-slate-950/80 p-3 rounded-lg border border-slate-900"></pre>
              </div>
            </div>
          </div>

          <!-- Tab: Operator Plan View -->
          <div id="viewExplain" class="hidden bg-white border border-slate-200/90 rounded-2xl overflow-hidden shadow-sm flex flex-col p-5">
            <div class="flex justify-between items-center mb-3">
              <div>
                <h2 class="text-xs font-bold text-sky-700 uppercase tracking-wider flex items-center gap-2">
                  <svg class="w-4 h-4 text-sky-600" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2m-3 7h3m-3 4h3m-6-4h.01M9 16h.01"/></svg>
                  <span>Physical Operator Execution Plan</span>
                </h2>
                <p class="text-[11px] text-slate-500 mt-0.5">Built by buildPlan() in src/planner.cpp with filter pushdown and join reordering</p>
              </div>
              <button onclick="updatePlanView()" class="text-xs bg-slate-100 hover:bg-slate-200 text-slate-700 border border-slate-200 px-3 py-1.5 rounded-lg flex items-center gap-1.5 font-medium">
                <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15"/></svg>
                <span>Refresh Plan</span>
              </button>
            </div>
            <pre id="explainOutput" class="code-font text-xs bg-[#0b0f19] p-4 rounded-xl border border-slate-800 text-sky-300 overflow-x-auto leading-relaxed h-[380px] whitespace-pre"></pre>
          </div>

        </div>

      </div>

    </main>

  </div>

  <!-- Notification Toast -->
  <div id="toast" class="fixed bottom-6 right-6 z-50 bg-slate-900 border border-slate-800 text-white px-4 py-2.5 rounded-xl shadow-2xl transition-all duration-300 transform translate-y-12 opacity-0 flex items-center gap-2 text-xs font-semibold">
    <span id="toastIcon">
      <svg class="w-4 h-4 text-sky-400" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 16h-1v-4h-1m1-4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/></svg>
    </span>
    <span id="toastMsg">Ready</span>
  </div>

  <script>
    let editorInstance = null;
    let latestData = null;

    const PRESETS = {
      "S4": {
        instructions: "Select city, count of transactions, and total sum of amount from table t, grouped by city, ordered by city, limited to top 10 records.",
        sql: "SELECT city, COUNT(*), SUM(amount) FROM t GROUP BY city ORDER BY city LIMIT 10",
        engMs: 14.92, sqMs: 28.38, speedup: "1.90x", speedupLbl: "+90.0% FASTER",
        engPct: 34, sqPct: 66,
        ast: "(select (distinct false)\n  (items (col city) (agg count *) (agg sum (col amount)))\n  (from t)\n  (group_by (col city))\n  (order_by (((col city) asc)))\n  (limit 10))",
        canonical: "SELECT city, count(*), sum(amount) FROM t GROUP BY city ORDER BY city ASC LIMIT 10",
        plan: "Scan (table: t, file: tests/data/data_100k.csv, needed_columns: [city, amount])\n  Aggregate (group_by: [city], aggregates: [count(*), sum(amount)])\n    Project (columns: [city, count(*), sum(amount)], visible_width: 3, hidden_columns: 0)\n      Sort (terms: [city ASC], mode: TOP-N (n: 10))\n        Limit (limit: 10, offset: 0)"
      },
      "S8": {
        instructions: "Retrieve all unique city names from dataset t ordered alphabetically, limited to top 20.",
        sql: "SELECT DISTINCT city FROM t ORDER BY city LIMIT 20",
        engMs: 10.03, sqMs: 26.08, speedup: "2.60x", speedupLbl: "+160.0% FASTER",
        engPct: 28, sqPct: 72,
        ast: "(select (distinct true)\n  (items (col city))\n  (from t)\n  (order_by (((col city) asc)))\n  (limit 20))",
        canonical: "SELECT DISTINCT city FROM t ORDER BY city ASC LIMIT 20",
        plan: "Scan (table: t, file: tests/data/data_100k.csv, needed_columns: [city])\n  Distinct (columns: [city])\n    Sort (terms: [city ASC], mode: TOP-N (n: 20))\n      Limit (limit: 20, offset: 0)"
      },
      "S6": {
        instructions: "Find the top 10 highest-value records in table t with id and amount, sorted descending.",
        sql: "SELECT id, amount FROM t ORDER BY amount DESC, id LIMIT 10",
        engMs: 11.97, sqMs: 3.24, speedup: "0.27x", speedupLbl: "PARITY (Process startup)",
        engPct: 78, sqPct: 22,
        ast: "(select (distinct false)\n  (items (col id) (col amount))\n  (from t)\n  (order_by (((col amount) desc) ((col id) asc)))\n  (limit 10))",
        canonical: "SELECT id, amount FROM t ORDER BY amount DESC, id ASC LIMIT 10",
        plan: "Scan (table: t, file: tests/data/data_100k.csv, needed_columns: [id, amount])\n  Sort (terms: [amount DESC, id ASC], mode: TOP-N (n: 10))\n    Limit (limit: 10, offset: 0)"
      },
      "J1": {
        instructions: "Count total matched orders and customers using an inner hash join on customer ID.",
        sql: "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id",
        engMs: 1.79, sqMs: 0.20, speedup: "0.11x", speedupLbl: "PARITY (0.001s)",
        engPct: 90, sqPct: 10,
        ast: "(select (distinct false)\n  (items (agg count *))\n  (from orders as o)\n  (joins (join inner customers as c (on (= (col o cust_id) (col c id))))))",
        canonical: "SELECT count(*) FROM orders AS o INNER JOIN customers AS c ON (o.cust_id = c.id)",
        plan: "Joins (1 joins):\n  Join 0 (type: INNER, probe: orders, build: customers, mode: HASH, equi_keys: [o.cust_id = c.id])\n  Aggregate (aggregates: [count(*)])\n    Project (columns: [count(*)])"
      },
      "J6": {
        instructions: "Perform a full outer join between orders and customers, counting both matched and unmatched records.",
        sql: "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id",
        engMs: 1.48, sqMs: 3.60, speedup: "2.44x", speedupLbl: "+144.0% FASTER",
        engPct: 29, sqPct: 71,
        ast: "(select (distinct false)\n  (items (agg count *) (agg count (col o id)) (agg count (col c id)))\n  (from orders as o)\n  (joins (join full customers as c (on (= (col o cust_id) (col c id))))))",
        canonical: "SELECT count(*), count(o.id), count(c.id) FROM orders AS o FULL JOIN customers AS c ON (o.cust_id = c.id)",
        plan: "Joins (1 joins):\n  Join 0 (type: FULL, probe: orders, build: customers, mode: HASH, equi_keys: [o.cust_id = c.id], emit_unmatched_probe: yes, emit_unmatched_build: yes)\n  Aggregate (aggregates: [count(*), count(o.id), count(c.id)])"
      },
      "S1": {
        instructions: "Filter records from table t where amount is greater than 500, returning id, city, and amount.",
        sql: "SELECT id, city, amount FROM t WHERE amount > 500 LIMIT 10",
        engMs: 8.42, sqMs: 14.12, speedup: "1.68x", speedupLbl: "+68.0% FASTER",
        engPct: 37, sqPct: 63,
        ast: "(select (distinct false)\n  (items (col id) (col city) (col amount))\n  (from t)\n  (where (> (col amount) (int 500)))\n  (limit 10))",
        canonical: "SELECT id, city, amount FROM t WHERE (amount > 500) LIMIT 10",
        plan: "Scan (table: t, file: tests/data/data_100k.csv, needed_columns: [id, city, amount], filter: [amount > 500])\n  Project (columns: [id, city, amount])\n    Limit (limit: 10, offset: 0)"
      }
    };

    function initEditor() {
      const textarea = document.getElementById("sqlInput");
      if (typeof CodeMirror !== "undefined") {
        editorInstance = CodeMirror.fromTextArea(textarea, {
          mode: "text/x-sql",
          theme: "sqlai",
          lineNumbers: true,
          matchBrackets: true,
          autoCloseBrackets: true,
          indentUnit: 2,
          tabSize: 2,
          lineWrapping: true,
          extraKeys: {
            "Ctrl-Enter": function() { runQuery(); },
            "Cmd-Enter": function() { runQuery(); }
          }
        });
      }
    }

    function getSQL() {
      return editorInstance ? editorInstance.getValue().trim() : document.getElementById("sqlInput").value.trim();
    }

    function setSQL(val) {
      if (editorInstance) editorInstance.setValue(val);
      else document.getElementById("sqlInput").value = val;
    }

    function insertColumn(col) {
      if (editorInstance) {
        const doc = editorInstance.getDoc();
        const cursor = doc.getCursor();
        doc.replaceRange(" " + col + " ", cursor);
        editorInstance.focus();
      } else {
        const ta = document.getElementById("sqlInput");
        ta.value += " " + col;
      }
      showToast("Inserted " + col);
    }

    function applyPreset() {
      const q = document.getElementById("presetSelect").value;
      if (PRESETS[q]) {
        document.getElementById("instructionsText").value = PRESETS[q].instructions;
        setSQL(PRESETS[q].sql);
        runQuery();
      }
    }

    function toggleAccordion(id) {
      const el = document.getElementById(id);
      const icon = document.getElementById("icon_" + id);
      if (el.classList.contains("hidden")) {
        el.classList.remove("hidden");
        if (icon) icon.style.transform = "rotate(180deg)";
      } else {
        el.classList.add("hidden");
        if (icon) icon.style.transform = "rotate(0deg)";
      }
    }

    function filterDatasource(val) {
      const q = val.toLowerCase().trim();
      document.querySelectorAll("#datasourceList > div").forEach(div => {
        div.style.display = !q || div.innerText.toLowerCase().includes(q) ? "" : "none";
      });
    }

    // Highlighting parser for AST S-Expressions
    function highlightSExpr(sexpr) {
      if (!sexpr) return "<span class='text-slate-500 italic'>No AST available</span>";
      let out = "";
      let i = 0;
      const n = sexpr.length;

      while (i < n) {
        const ch = sexpr[i];
        if (ch === '(') {
          out += '<span class="text-slate-500 font-bold">(</span>';
          i++;
          let tag = "";
          while (i < n && /[a-zA-Z0-9_=<>]/.test(sexpr[i])) {
            tag += sexpr[i++];
          }
          if (tag) {
            const lower = tag.toLowerCase();
            const clauses = ["select", "items", "from", "joins", "join", "where", "group_by", "having", "order_by", "limit", "offset", "distinct"];
            const aggs = ["agg", "count", "sum", "avg", "min", "max"];
            const refs = ["col", "table", "as", "on"];
            const lits = ["int", "float", "str", "null", "true", "false"];
            const ops = ["=", "!=", "<", "<=", ">", ">=", "and", "or", "not", "like", "in", "is", "asc", "desc", "inner", "left", "right", "full", "cross"];

            if (clauses.includes(lower)) {
              out += `<span class="text-rose-400 font-bold">${escapeHtml(tag)}</span>`;
            } else if (aggs.includes(lower)) {
              out += `<span class="text-indigo-400 font-semibold">${escapeHtml(tag)}</span>`;
            } else if (refs.includes(lower)) {
              out += `<span class="text-cyan-300 font-medium">${escapeHtml(tag)}</span>`;
            } else if (lits.includes(lower)) {
              out += `<span class="text-emerald-400 font-medium">${escapeHtml(tag)}</span>`;
            } else if (ops.includes(lower)) {
              out += `<span class="text-amber-400 font-medium">${escapeHtml(tag)}</span>`;
            } else {
              out += `<span class="text-slate-300">${escapeHtml(tag)}</span>`;
            }
          }
        } else if (ch === ')') {
          out += '<span class="text-slate-500 font-bold">)</span>';
          i++;
        } else if (ch === '"' || ch === "'") {
          const quote = ch;
          let lit = quote;
          i++;
          while (i < n && sexpr[i] !== quote) {
            if (sexpr[i] === '\\' && i + 1 < n) lit += sexpr[i++];
            lit += sexpr[i++];
          }
          if (i < n) lit += sexpr[i++];
          out += `<span class="text-emerald-300 font-mono">${escapeHtml(lit)}</span>`;
        } else if (ch === '*' && (i === 0 || /[\s()]/.test(sexpr[i-1]))) {
          out += '<span class="text-amber-400 font-bold">*</span>';
          i++;
        } else if (/[0-9]/.test(ch) && (i === 0 || /[\s()]/.test(sexpr[i-1]))) {
          let num = "";
          while (i < n && /[0-9.\-+]/.test(sexpr[i])) num += sexpr[i++];
          out += `<span class="text-emerald-400 font-mono">${num}</span>`;
        } else if (ch === '\n') {
          out += '\n';
          i++;
        } else if (/\s/.test(ch)) {
          out += ch;
          i++;
        } else {
          let word = "";
          while (i < n && !/[\s()]/.test(sexpr[i])) {
            word += sexpr[i++];
          }
          const lower = word.toLowerCase();
          if (["asc", "desc", "true", "false", "inner", "left", "right", "full", "cross"].includes(lower)) {
            out += `<span class="text-amber-300 font-medium">${escapeHtml(word)}</span>`;
          } else {
            out += `<span class="text-slate-200">${escapeHtml(word)}</span>`;
          }
        }
      }
      return out;
    }

    function renderAst(astData) {
      const astOutput = document.getElementById("astOutput");
      const canonicalSqlOutput = document.getElementById("canonicalSqlOutput");
      const summaryCards = document.getElementById("astSummaryCards");

      if (!astData || astData.error) {
        astOutput.innerHTML = `<div class="p-4 rounded-lg bg-rose-950/40 border border-rose-800/60 text-rose-300 font-mono text-xs whitespace-pre-wrap"><div class="text-rose-400 font-bold mb-2 flex items-center gap-2"><svg class="w-4 h-4 text-rose-400 inline" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-3L13.732 4c-.77-1.333-2.694-1.333-3.464 0L3.34 16c-.77 1.333.192 3 1.732 3z"/></svg> Syntax / Parser Error</div>${escapeHtml(astData ? astData.error : "Unknown parse error")}</div>`;
        canonicalSqlOutput.innerText = "(Unavailable due to parse error)";
        summaryCards.innerHTML = `
          <div class="col-span-full bg-rose-50 p-2.5 rounded-lg border border-rose-200 text-xs text-rose-700 font-mono flex items-center gap-2">
            <span>[Error]</span> C++ SqlParser rejected the query. Check SQL syntax, clauses, or matching parentheses.
          </div>
        `;
        return;
      }

      const sexpr = astData.ast_sexpr || "";
      astOutput.innerHTML = highlightSExpr(sexpr);
      canonicalSqlOutput.innerText = astData.canonical_sql || "(Empty canonical SQL)";

      const isDistinct = sexpr.includes("(distinct true)");
      const hasJoins = sexpr.includes("(joins");
      const hasWhere = sexpr.includes("(where");
      const hasGroupBy = sexpr.includes("(group_by");
      const hasHaving = sexpr.includes("(having");
      const hasOrderBy = sexpr.includes("(order_by");
      const hasLimit = sexpr.includes("(limit");

      summaryCards.innerHTML = `
        <div class="bg-slate-50 p-2.5 rounded-lg border border-slate-200">
          <div class="text-[10px] text-slate-500 uppercase font-semibold">Statement</div>
          <div class="text-rose-600 font-bold">SELECT ${isDistinct ? 'DISTINCT' : 'ALL'}</div>
        </div>
        <div class="bg-slate-50 p-2.5 rounded-lg border border-slate-200">
          <div class="text-[10px] text-slate-500 uppercase font-semibold">From & Joins</div>
          <div class="text-indigo-600 font-bold">${hasJoins ? "Hash Join" : "Single Scan"}</div>
        </div>
        <div class="bg-slate-50 p-2.5 rounded-lg border border-slate-200">
          <div class="text-[10px] text-slate-500 uppercase font-semibold">Filter Predicate</div>
          <div class="${hasWhere ? 'text-emerald-600' : 'text-slate-400'} font-bold">${hasWhere ? "WHERE (Active)" : "No Filter"}</div>
        </div>
        <div class="bg-slate-50 p-2.5 rounded-lg border border-slate-200">
          <div class="text-[10px] text-slate-500 uppercase font-semibold">Aggregation</div>
          <div class="text-emerald-600 font-bold">${hasGroupBy ? "GROUP BY" : (sexpr.includes("(agg") ? "Global Agg" : "None")} ${hasHaving ? "+ HAVING" : ""}</div>
        </div>
        <div class="bg-slate-50 p-2.5 rounded-lg border border-slate-200">
          <div class="text-[10px] text-slate-500 uppercase font-semibold">Ordering & Window</div>
          <div class="text-amber-600 font-bold">${hasOrderBy ? "ORDER BY" : "Natural"} ${hasLimit ? "+ LIMIT" : ""}</div>
        </div>
      `;
    }

    async function updateAstView() {
      const sql = getSQL();
      if (!sql) return;

      try {
        const res = await fetch("/api/ast", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sql })
        });
        if (res.ok) {
          const data = await res.json();
          if (!latestData) latestData = {};
          latestData.ast = data;
          latestData.sql = sql;
          renderAst(data);
          return;
        }
      } catch (e) {}

      for (const k in PRESETS) {
        if (PRESETS[k].sql.toLowerCase().replace(/\s+/g, ' ') === sql.toLowerCase().replace(/\s+/g, ' ')) {
          renderAst({ ast_sexpr: PRESETS[k].ast, canonical_sql: PRESETS[k].canonical });
          return;
        }
      }
    }

    async function updatePlanView() {
      const sql = getSQL();
      if (!sql) return;
      const dataset = document.getElementById("datasetSelect").value;
      const explainPre = document.getElementById("explainOutput");
      explainPre.innerText = "Computing physical operator tree via ./engine --explain...";

      try {
        const res = await fetch("/api/explain", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sql, dataset })
        });
        if (res.ok) {
          const data = await res.json();
          explainPre.innerText = data.error ? ("Plan Error:\n" + data.error) : (data.plan || "Empty plan");
          return;
        }
      } catch (e) {}

      for (const k in PRESETS) {
        if (PRESETS[k].sql.toLowerCase().replace(/\s+/g, ' ') === sql.toLowerCase().replace(/\s+/g, ' ')) {
          explainPre.innerText = PRESETS[k].plan;
          return;
        }
      }
    }

    async function runQuery(silent = false) {
      const sql = getSQL();
      if (!sql) {
        showToast("Please enter a SQL query");
        return;
      }

      const dataset = document.getElementById("datasetSelect").value;
      const btn = document.getElementById("runBtn");
      const icon = document.getElementById("runBtnIcon");
      const text = document.getElementById("runBtnText");
      const statusText = document.getElementById("runStatusText");

      btn.disabled = true;
      icon.innerHTML = `<svg class="animate-spin h-3.5 w-3.5 text-white inline" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8v8H4z"></path></svg>`;
      text.innerText = "Executing...";
      statusText.innerText = "Executing in C++ SIMD engine...";

      try {
        const res = await fetch("/api/execute", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sql, dataset, options: { memory_limit_mb: 512 } })
        });

        if (res.ok) {
          const data = await res.json();
          latestData = data;
          latestData.sql = sql;
          renderResults(data);
          updateAstView();
          updatePlanView();
          if (!silent) showToast("Completed in " + data.engine.elapsed_ms.toFixed(1) + " ms");
          return;
        }
      } catch (e) {
        for (const k in PRESETS) {
          if (PRESETS[k].sql.toLowerCase().replace(/\s+/g, ' ') === sql.toLowerCase().replace(/\s+/g, ' ')) {
            const p = PRESETS[k];
            document.getElementById("engTime").innerText = p.engMs.toFixed(2) + " ms";
            document.getElementById("sqTime").innerText = p.sqMs.toFixed(2) + " ms";
            document.getElementById("speedup").innerText = p.speedup;
            document.getElementById("speedupLabel").innerText = p.speedupLbl;
            document.getElementById("barEngVal").innerText = `${p.engMs.toFixed(2)} ms (${p.engPct}%)`;
            document.getElementById("barSqVal").innerText = `${p.sqMs.toFixed(2)} ms (${p.sqPct}%)`;
            document.getElementById("barEng").style.width = p.engPct + "%";
            document.getElementById("barSq").style.width = p.sqPct + "%";
            renderAst({ ast_sexpr: p.ast, canonical_sql: p.canonical });
            document.getElementById("explainOutput").innerText = p.plan;
            break;
          }
        }
      } finally {
        btn.disabled = false;
        icon.innerHTML = `<svg class="w-3.5 h-3.5 fill-current" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>`;
        text.innerText = "Generate query";
        statusText.innerText = "";
      }
    }

    function renderResults(data) {
      if (data.error) {
        showToast("Error: " + data.error);
        return;
      }

      const engMs = data.engine.elapsed_ms;
      const sqMs = data.sqlite.elapsed_ms;

      document.getElementById("engTime").innerText = engMs < 1.0 ? engMs.toFixed(3) + " ms" : engMs.toFixed(2) + " ms";
      document.getElementById("sqTime").innerText = sqMs < 1.0 ? sqMs.toFixed(3) + " ms" : sqMs.toFixed(2) + " ms";
      document.getElementById("engRows").innerText = data.engine.rows.length.toLocaleString() + " rows";
      document.getElementById("sqRows").innerText = data.sqlite.rows.length.toLocaleString() + " rows";

      const speedup = data.speedup;
      document.getElementById("speedup").innerText = speedup.toFixed(2) + "x";
      if (speedup >= 1.05) {
        document.getElementById("speedupLabel").innerText = "+" + ((speedup - 1.0) * 100.0).toFixed(0) + "% FASTER";
      } else {
        document.getElementById("speedupLabel").innerText = "PARITY";
      }

      const totalTime = Math.max(engMs + sqMs, 0.001);
      const engPct = Math.round((engMs / totalTime) * 100);
      const sqPct = 100 - engPct;
      document.getElementById("barEngVal").innerText = `${engMs.toFixed(2)} ms (${engPct}%)`;
      document.getElementById("barSqVal").innerText = `${sqMs.toFixed(2)} ms (${sqPct}%)`;
      document.getElementById("barEng").style.width = engPct + "%";
      document.getElementById("barSq").style.width = sqPct + "%";

      const badge = document.getElementById("matchBadge");
      const desc = document.getElementById("matchDesc");
      if (data.matched) {
        badge.innerHTML = `<svg class="w-4 h-4 text-emerald-600 inline mr-1" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2.5" d="M5 13l4 4L19 7"/></svg><span class="text-emerald-600 font-bold">EXACT MATCH</span>`;
        desc.innerText = `All ${data.engine.rows.length.toLocaleString()} rows match SQLite oracle`;
      } else {
        badge.innerHTML = `<span class="text-rose-600 font-bold">MISMATCH</span>`;
        desc.innerText = data.diff_reason || "Results differ from SQLite";
      }

      currentPage = 1;
      renderCurrentPage();
      if (data.ast) renderAst(data.ast);
    }

    const PAGE_SIZE = 250;
    let currentPage = 1;

    function goToPage(pageNum) {
      if (!latestData) return;
      const engLen = (latestData.engine && latestData.engine.rows) ? latestData.engine.rows.length : 0;
      const sqLen = (latestData.sqlite && latestData.sqlite.rows) ? latestData.sqlite.rows.length : 0;
      const maxRows = Math.max(engLen, sqLen);
      const totalPages = Math.max(1, Math.ceil(maxRows / PAGE_SIZE));

      let p = parseInt(pageNum, 10);
      if (isNaN(p) || p < 1) p = 1;
      if (p > totalPages) p = totalPages;

      currentPage = p;
      renderCurrentPage();
    }

    function renderPagination(containerId, currPage, totalRows, pageSize) {
      const container = document.getElementById(containerId);
      if (!container) return;

      if (!totalRows || totalRows === 0) {
        container.innerHTML = `<div class="text-xs text-slate-400 italic">0 entries</div>`;
        return;
      }

      const totalPages = Math.max(1, Math.ceil(totalRows / pageSize));
      const startIdx = (currPage - 1) * pageSize;
      const endIdx = Math.min(startIdx + pageSize, totalRows);

      let pages = [];
      if (totalPages <= 7) {
        for (let p = 1; p <= totalPages; p++) pages.push(p);
      } else {
        pages.push(1);
        if (currPage > 3) pages.push("...");
        const start = Math.max(2, currPage - 1);
        const end = Math.min(totalPages - 1, currPage + 1);
        for (let p = start; p <= end; p++) pages.push(p);
        if (currPage < totalPages - 2) pages.push("...");
        pages.push(totalPages);
      }

      let html = `
        <div class="flex items-center gap-1.5 text-xs text-slate-500 font-medium">
          <span>Showing</span>
          <span class="font-semibold text-slate-800">${(startIdx + 1).toLocaleString()}</span>
          <span>–</span>
          <span class="font-semibold text-slate-800">${endIdx.toLocaleString()}</span>
          <span>of</span>
          <span class="font-semibold text-slate-800">${totalRows.toLocaleString()}</span>
          <span>entries</span>
          <span class="text-slate-400 text-[11px] ml-1">(${pageSize}/page)</span>
        </div>

        <div class="flex items-center gap-1 text-xs">
          <button onclick="goToPage(1)" ${currPage === 1 ? "disabled" : ""} class="px-2 py-1 rounded-md border border-slate-200 bg-white hover:bg-slate-100 disabled:opacity-30 disabled:pointer-events-none transition flex items-center text-slate-700" title="First Page">
            <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M11 19l-7-7 7-7m8 14l-7-7 7-7"/></svg>
          </button>
          <button onclick="goToPage(${currPage - 1})" ${currPage === 1 ? "disabled" : ""} class="px-2 py-1 rounded-md border border-slate-200 bg-white hover:bg-slate-100 disabled:opacity-30 disabled:pointer-events-none transition flex items-center text-slate-700" title="Previous Page">
            <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15 19l-7-7 7-7"/></svg>
          </button>
      `;

      pages.forEach(p => {
        if (p === "...") {
          html += `<span class="px-1.5 py-1 text-slate-400 font-mono">...</span>`;
        } else {
          const active = p === currPage;
          const cls = active
            ? "bg-slate-900 text-white font-bold border-slate-900 shadow-sm"
            : "bg-white hover:bg-slate-100 text-slate-700 border-slate-200";
          html += `<button onclick="goToPage(${p})" class="min-w-[28px] px-2 py-1 rounded-md border text-xs font-mono transition text-center ${cls}">${p}</button>`;
        }
      });

      html += `
          <button onclick="goToPage(${currPage + 1})" ${currPage === totalPages ? "disabled" : ""} class="px-2 py-1 rounded-md border border-slate-200 bg-white hover:bg-slate-100 disabled:opacity-30 disabled:pointer-events-none transition flex items-center text-slate-700" title="Next Page">
            <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M9 5l7 7-7 7"/></svg>
          </button>
          <button onclick="goToPage(${totalPages})" ${currPage === totalPages ? "disabled" : ""} class="px-2 py-1 rounded-md border border-slate-200 bg-white hover:bg-slate-100 disabled:opacity-30 disabled:pointer-events-none transition flex items-center text-slate-700" title="Last Page">
            <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M13 5l7 7-7 7M5 5l7 7-7 7"/></svg>
          </button>
      `;

      if (totalPages > 5) {
        html += `
          <div class="flex items-center gap-1 ml-2 text-slate-500 font-medium">
            <span>Page</span>
            <input type="number" min="1" max="${totalPages}" value="${currPage}" onkeydown="if(event.key==='Enter') goToPage(parseInt(this.value))" onchange="goToPage(parseInt(this.value))" class="w-12 px-1.5 py-0.5 text-center text-xs border border-slate-200 rounded font-mono bg-white text-slate-800 outline-none">
            <span class="text-slate-400">/ ${totalPages}</span>
          </div>
        `;
      }

      html += `</div>`;
      container.innerHTML = html;
    }

    function renderCurrentPage() {
      if (!latestData) return;
      if (latestData.engine) {
        renderTable("engTable", "engPagination", "engRowCount", latestData.engine.header, latestData.engine.rows);
      }
      if (latestData.sqlite) {
        renderTable("sqTable", "sqPagination", "sqRowCount", latestData.sqlite.header, latestData.sqlite.rows);
      }
      renderDiffTable(latestData);
    }

    function renderTable(tableId, paginationId, rowCountId, headers, rows) {
      const tbl = document.getElementById(tableId);
      if (!tbl) return;
      tbl.innerHTML = "";
      const totalRows = rows ? rows.length : 0;
      const totalPages = Math.max(1, Math.ceil(totalRows / PAGE_SIZE));

      const countEl = document.getElementById(rowCountId);
      if (countEl) {
        if (totalRows <= PAGE_SIZE) {
          countEl.innerText = `${totalRows.toLocaleString()} rows`;
        } else {
          countEl.innerText = `${totalRows.toLocaleString()} rows (Page ${currentPage}/${totalPages})`;
        }
      }

      if (!headers || headers.length === 0 || totalRows === 0) {
        tbl.innerHTML = "<tbody><tr class='text-slate-400 italic'><td class='p-3 text-center'>Empty result set</td></tr></tbody>";
        renderPagination(paginationId, 1, 0, PAGE_SIZE);
        return;
      }

      const startIdx = (currentPage - 1) * PAGE_SIZE;
      const endIdx = Math.min(startIdx + PAGE_SIZE, totalRows);
      const pageRows = rows.slice(startIdx, endIdx);

      let headerHtml = "<thead class='bg-slate-50 sticky top-0 border-b border-slate-200 z-10 text-[11px] text-slate-500 uppercase'><tr>";
      headerHtml += `<th class="p-2 border-r border-slate-200 text-slate-400 text-center w-12">#</th>`;
      headers.forEach(h => {
        headerHtml += `<th class="p-2 text-slate-700 font-semibold whitespace-nowrap">${escapeHtml(h)}</th>`;
      });
      headerHtml += "</tr></thead>";

      let bodyHtml = "<tbody class='divide-y divide-slate-100 text-slate-700 text-xs'>";
      for (let i = 0; i < pageRows.length; ++i) {
        const globalRowIdx = startIdx + i + 1;
        bodyHtml += "<tr class='hover:bg-slate-50'>";
        bodyHtml += `<td class="p-2 border-r border-slate-100 text-slate-400 text-center font-mono text-[11px]">${globalRowIdx.toLocaleString()}</td>`;
        pageRows[i].forEach(val => {
          if (val === "" || val === null) {
            bodyHtml += `<td class="p-2 whitespace-nowrap"><span class="badge-null">NULL</span></td>`;
          } else {
            bodyHtml += `<td class="p-2 whitespace-nowrap">${escapeHtml(String(val))}</td>`;
          }
        });
        bodyHtml += "</tr>";
      }
      bodyHtml += "</tbody>";
      tbl.innerHTML = headerHtml + bodyHtml;

      renderPagination(paginationId, currentPage, totalRows, PAGE_SIZE);
    }

    function renderDiffTable(data) {
      const tbl = document.getElementById("diffTable");
      if (!tbl) return;
      tbl.innerHTML = "";
      const engRows = (data.engine && data.engine.rows) ? data.engine.rows : [];
      const sqRows = (data.sqlite && data.sqlite.rows) ? data.sqlite.rows : [];
      const engHeader = (data.engine && data.engine.header) ? data.engine.header : [];
      const sqHeader = (data.sqlite && data.sqlite.header) ? data.sqlite.header : [];
      const headers = engHeader.length ? engHeader : sqHeader;

      const totalDiffRows = Math.max(engRows.length, sqRows.length);
      const totalPages = Math.max(1, Math.ceil(totalDiffRows / PAGE_SIZE));

      const summaryBadge = document.getElementById("diffSummaryBadge");
      if (summaryBadge) {
        summaryBadge.innerText = data.matched
          ? `100% Matching (${totalDiffRows.toLocaleString()} rows)`
          : "Mismatch Detected";
        summaryBadge.className = data.matched
          ? "text-xs font-mono text-emerald-600 font-semibold"
          : "text-xs font-mono text-rose-600 font-semibold";
      }

      if (headers.length === 0 || totalDiffRows === 0) {
        tbl.innerHTML = "<tbody><tr class='text-slate-400 italic'><td class='p-3 text-center'>No diff data available</td></tr></tbody>";
        renderPagination("diffPagination", 1, 0, PAGE_SIZE);
        return;
      }

      let headerHtml = "<thead class='bg-slate-50 sticky top-0 border-b border-slate-200 z-10 text-[11px] text-slate-500'><tr>";
      headerHtml += `<th class="p-2 text-center w-12 border-r border-slate-200">#</th><th class="p-2 text-center w-16 border-r border-slate-200">Status</th>`;
      headers.forEach(h => {
        headerHtml += `<th class="p-2 border-r border-slate-200 text-sky-700 whitespace-nowrap">Engine: ${escapeHtml(h)}</th>`;
        headerHtml += `<th class="p-2 border-r border-slate-200 text-indigo-700 whitespace-nowrap">SQLite: ${escapeHtml(h)}</th>`;
      });
      headerHtml += "</tr></thead>";

      const startIdx = (currentPage - 1) * PAGE_SIZE;
      const endIdx = Math.min(startIdx + PAGE_SIZE, totalDiffRows);

      let bodyHtml = "<tbody class='divide-y divide-slate-100 text-xs'>";
      for (let i = startIdx; i < endIdx; ++i) {
        const eRow = i < engRows.length ? engRows[i] : null;
        const sRow = i < sqRows.length ? sqRows[i] : null;
        let rowMatches = true;

        if (!eRow || !sRow || eRow.length !== sRow.length) rowMatches = false;
        else {
          for (let j = 0; j < eRow.length; ++j) {
            if (eRow[j] !== sRow[j]) {
              const fe = parseFloat(eRow[j]), fs = parseFloat(sRow[j]);
              if (isNaN(fe) || isNaN(fs) || Math.abs(fe - fs) >= 1e-6) {
                rowMatches = false; break;
              }
            }
          }
        }

        const trClass = rowMatches ? "hover:bg-slate-50" : "bg-rose-50 hover:bg-rose-100";
        bodyHtml += `<tr class="${trClass}">`;
        bodyHtml += `<td class="p-2 text-center font-mono text-slate-400 border-r border-slate-100">${(i + 1).toLocaleString()}</td>`;
        bodyHtml += `<td class="p-2 text-center border-r border-slate-100">${rowMatches ? '<span class="text-emerald-600 font-bold">MATCH</span>' : '<span class="text-rose-600 font-bold">DIFF</span>'}</td>`;

        for (let j = 0; j < headers.length; ++j) {
          const ev = eRow && j < eRow.length ? eRow[j] : "";
          const sv = sRow && j < sRow.length ? sRow[j] : "";
          const cellMatches = ev === sv || (!isNaN(parseFloat(ev)) && !isNaN(parseFloat(sv)) && Math.abs(parseFloat(ev) - parseFloat(sv)) < 1e-6);
          const cellClass = cellMatches ? "" : "bg-rose-100 text-rose-800 font-semibold";
          bodyHtml += `<td class="p-2 border-r border-slate-100 whitespace-nowrap ${cellClass}">${ev === "" ? '<span class="badge-null">NULL</span>' : escapeHtml(String(ev))}</td>`;
          bodyHtml += `<td class="p-2 border-r border-slate-100 whitespace-nowrap ${cellClass}">${sv === "" ? '<span class="badge-null">NULL</span>' : escapeHtml(String(sv))}</td>`;
        }
        bodyHtml += `</tr>`;
      }
      bodyHtml += "</tbody>";
      tbl.innerHTML = headerHtml + bodyHtml;

      renderPagination("diffPagination", currentPage, totalDiffRows, PAGE_SIZE);
    }

    function switchView(viewName) {
      const tabs = ["SideBySide", "Diff", "Ast", "Explain"];
      tabs.forEach(t => {
        const btn = document.getElementById("tab" + t);
        const view = document.getElementById("view" + t);
        if (btn) btn.className = "bg-white border border-slate-200 text-slate-700 hover:bg-slate-50 px-3 py-1.5 rounded-full text-xs font-medium shadow-sm transition flex items-center gap-1.5";
        if (view) view.classList.add("hidden");
      });

      const cap = viewName.charAt(0).toUpperCase() + viewName.slice(1);
      const activeBtn = document.getElementById("tab" + cap);
      const activeView = document.getElementById("view" + cap);
      if (activeBtn) activeBtn.className = "bg-slate-900 text-white font-semibold px-3 py-1.5 rounded-full text-xs shadow-sm transition flex items-center gap-1.5";
      if (activeView) activeView.classList.remove("hidden");

      if (viewName === "ast") {
        updateAstView();
      } else if (viewName === "explain") {
        updatePlanView();
      }
    }

    function formatEditorSql() {
      const sql = getSQL();
      let s = sql;
      const kw = ["SELECT", "FROM", "WHERE", "GROUP BY", "HAVING", "ORDER BY", "LIMIT", "JOIN", "INNER JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN", "ON", "COUNT", "SUM", "AVG", "MIN", "MAX", "DISTINCT", "EXISTS", "NOT EXISTS", "AND", "OR"];
      kw.forEach(k => {
        s = s.replace(new RegExp("\\b" + k + "\\b", "gi"), k);
      });
      ["FROM", "WHERE", "GROUP BY", "HAVING", "ORDER BY", "LIMIT", "JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN", "AND", "OR"].forEach(k => {
        s = s.replace(new RegExp("\\s+(" + k + ")\\b", "gi"), "\n$1");
      });
      setSQL(s.trim());
      showToast("SQL Formatted");
    }

    function copySQL() {
      navigator.clipboard.writeText(getSQL()).then(() => showToast("SQL copied to clipboard"));
    }

    function clearEditor() {
      setSQL("");
      if (editorInstance) editorInstance.focus();
    }

    function copyAstSExpr() {
      if (!latestData || !latestData.ast || !latestData.ast.ast_sexpr) {
        showToast("No AST available to copy");
        return;
      }
      navigator.clipboard.writeText(latestData.ast.ast_sexpr).then(() => showToast("AST copied"));
    }

    function copyCanonicalSql() {
      if (!latestData || !latestData.ast || !latestData.ast.canonical_sql) {
        showToast("No Canonical SQL available to copy");
        return;
      }
      navigator.clipboard.writeText(latestData.ast.canonical_sql).then(() => showToast("Canonical SQL copied"));
    }

    function showToast(msg) {
      const toast = document.getElementById("toast");
      document.getElementById("toastMsg").innerText = msg;
      toast.classList.remove("translate-y-12", "opacity-0");
      setTimeout(() => toast.classList.add("translate-y-12", "opacity-0"), 2200);
    }

    function escapeHtml(str) {
      if (str === null || str === undefined) return "";
      return String(str).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
    }

    window.onload = function() {
      initEditor();
      runQuery(true);
    };
  </script>
</body>
</html>
"""


class DatabaseManager:
    """Manages cached in-memory SQLite instances so query execution is measured instantly."""
    def __init__(self, repo_root):
        self.repo_root = repo_root
        self.data_dir = os.path.join(repo_root, "tests", "data")
        self.join_dir = os.path.join(self.data_dir, "join_data")
        self.uploads_dir = os.path.join(self.data_dir, "uploads")
        os.makedirs(self.uploads_dir, exist_ok=True)
        self._connections = {}
        self._lock = threading.Lock()

    def get_connection(self, dataset="100k"):
        with self._lock:
            if dataset in self._connections:
                return self._connections[dataset]

            conn = sqlite3.connect(":memory:", check_same_thread=False)

            # Single table dataset
            if dataset == "100k":
                t_csv = os.path.join(self.data_dir, "data_100k.csv")
            elif dataset == "5m":
                t_csv = os.path.join(self.data_dir, "data_5m.csv")
            else:
                t_csv = os.path.join(self.data_dir, "data_small.csv")

            if os.path.exists(t_csv):
                load_csv_into_sqlite(t_csv, conn, "t")

            # Load multi-table join datasets
            orders_csv = os.path.join(self.join_dir, "orders.csv")
            customers_csv = os.path.join(self.join_dir, "customers.csv")
            products_csv = os.path.join(self.join_dir, "products.csv")

            if os.path.exists(orders_csv):
                load_csv_into_sqlite(orders_csv, conn, "orders")
            if os.path.exists(customers_csv):
                load_csv_into_sqlite(customers_csv, conn, "customers")
            if os.path.exists(products_csv):
                load_csv_into_sqlite(products_csv, conn, "products")

            # Load uploaded tables
            for up in glob.glob(os.path.join(self.uploads_dir, "*.csv")):
                alias = os.path.basename(up).replace(".csv", "")
                load_csv_into_sqlite(up, conn, alias)

            self._connections[dataset] = conn
            return conn

    def register_uploaded_table(self, file_path, alias):
        with self._lock:
            for conn in self._connections.values():
                try:
                    load_csv_into_sqlite(file_path, conn, alias)
                except Exception as e:
                    sys.stderr.write(f"Error loading table {alias} into cached SQLite: {e}\n")


class SideBySideHandler(http.server.BaseHTTPRequestHandler):
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    engine_bin = os.path.join(repo_root, "engine")
    sqlparse_bin = os.path.join(repo_root, "sqlparse")
    data_dir = os.path.join(repo_root, "tests", "data")
    join_dir = os.path.join(data_dir, "join_data")
    uploads_dir = os.path.join(data_dir, "uploads")
    db_manager = None

    def log_message(self, format, *args):
        sys.stderr.write(f"[{self.log_date_time_string()}] {args[0]} {args[1]}\n")

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path in ["/", "/index.html"]:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(HTML_TEMPLATE.encode("utf-8"))
        elif parsed.path == "/api/tables":
            self.send_tables()
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
        try:
            req_data = json.loads(body)
        except Exception:
            req_data = {}

        if parsed.path == "/api/execute":
            self.handle_execute(req_data)
        elif parsed.path == "/api/explain":
            self.handle_explain(req_data)
        elif parsed.path == "/api/ast":
            self.handle_ast(req_data)
        elif parsed.path == "/api/upload":
            self.handle_upload(req_data)
        else:
            self.send_response(404)
            self.end_headers()

    def get_table_schema_and_rows(self, csv_path):
        schema = []
        rows_count = 0
        try:
            p = subprocess.run([self.engine_bin, "--file", csv_path, "--schema"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
            if p.returncode == 0:
                for line in p.stdout.strip().split("\n"):
                    if ":" in line:
                        cname, ctype = line.split(":", 1)
                        schema.append({"name": cname.strip(), "type": ctype.strip()})
        except Exception:
            pass

        try:
            p = subprocess.run([self.engine_bin, "--file", csv_path, "--count"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
            if p.returncode == 0:
                for line in p.stdout.split("\n"):
                    if line.startswith("Rows:"):
                        rows_count = int(line.split(":", 1)[1].strip())
                        break
        except Exception:
            pass

        return schema, rows_count

    def send_tables(self):
        tables = {}
        predefined = [
            ("t (100k)", os.path.join(self.data_dir, "data_100k.csv")),
            ("t (small)", os.path.join(self.data_dir, "data_small.csv")),
            ("orders", os.path.join(self.join_dir, "orders.csv")),
            ("customers", os.path.join(self.join_dir, "customers.csv")),
            ("products", os.path.join(self.join_dir, "products.csv")),
        ]

        for fname in ["mid.csv", "nonote.csv"]:
            p = os.path.join(self.repo_root, fname)
            if os.path.exists(p):
                predefined.append((fname.replace(".csv", ""), p))

        for up in glob.glob(os.path.join(self.uploads_dir, "*.csv")):
            predefined.append((os.path.basename(up).replace(".csv", ""), up))

        for alias, path in predefined:
            if os.path.exists(path):
                sz = os.path.getsize(path)
                sz_str = f"{sz / 1024:.1f} KB" if sz < 1024*1024 else f"{sz / (1024*1024):.1f} MB"
                schema, row_count = self.get_table_schema_and_rows(path)
                tables[alias] = {
                    "path": os.path.relpath(path, self.repo_root),
                    "size_str": sz_str,
                    "rows": row_count,
                    "columns": schema
                }

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"tables": tables}).encode("utf-8"))

    def handle_upload(self, req_data):
        filename = req_data.get("filename", "upload.csv")
        alias = req_data.get("alias", "").strip() or filename.replace(".csv", "")
        alias = re.sub(r"[^a-zA-Z0-9_]", "_", alias).lower()
        content = req_data.get("content", "")

        dest_path = os.path.join(self.uploads_dir, f"{alias}.csv")
        with open(dest_path, "w", encoding="utf-8") as f:
            f.write(content)

        self.db_manager.register_uploaded_table(dest_path, alias)

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"success": True, "alias": alias, "path": dest_path}).encode("utf-8"))

    def build_engine_table_args(self, sql, dataset):
        sql_lower = sql.lower()
        table_args = []

        if any(tbl in sql_lower for tbl in ["orders", "customers", "products"]):
            table_args.extend([
                "--table", f"orders={os.path.join(self.join_dir, 'orders.csv')}",
                "--table", f"customers={os.path.join(self.join_dir, 'customers.csv')}",
                "--table", f"products={os.path.join(self.join_dir, 'products.csv')}",
            ])

        if "t" in sql_lower or not table_args:
            if dataset == "100k":
                csv_path = os.path.join(self.data_dir, "data_100k.csv")
            elif dataset == "5m":
                csv_path = os.path.join(self.data_dir, "data_5m.csv")
            else:
                csv_path = os.path.join(self.data_dir, "data_small.csv")
            table_args.extend(["--table", f"t={csv_path}"])

        for up in glob.glob(os.path.join(self.uploads_dir, "*.csv")):
            alias = os.path.basename(up).replace(".csv", "")
            if alias in sql_lower:
                table_args.extend(["--table", f"{alias}={up}"])

        return table_args

    def parse_ast_info(self, sql):
        """Parses AST S-expression and canonical SQL via sqlparse binary."""
        ast_sexpr = None
        canonical_sql = None
        err = None
        clean_sql = sql.strip().rstrip(";")
        try:
            p1 = subprocess.run([self.sqlparse_bin, clean_sql], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
            if p1.returncode == 0:
                ast_sexpr = p1.stdout.strip()
            else:
                err = p1.stderr.strip() or p1.stdout.strip() or f"Parser error code {p1.returncode}"
        except Exception as e:
            err = str(e)

        try:
            p2 = subprocess.run([self.sqlparse_bin, "--to-sql", clean_sql], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
            if p2.returncode == 0:
                canonical_sql = p2.stdout.strip()
        except Exception:
            pass

        return {"ast_sexpr": ast_sexpr, "canonical_sql": canonical_sql, "error": err}

    def handle_ast(self, req_data):
        sql = req_data.get("sql", "").strip()
        if not sql:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(json.dumps({"error": "Empty SQL query"}).encode("utf-8"))
            return

        resp = self.parse_ast_info(sql)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))

    def handle_explain(self, req_data):
        sql = req_data.get("sql", "").strip()
        dataset = req_data.get("dataset", "100k")

        table_args = self.build_engine_table_args(sql, dataset)
        cmd = [self.engine_bin] + table_args + ["--sql", sql, "--explain"]

        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10)
            if p.returncode == 0:
                resp = {"plan": p.stdout.strip(), "error": None}
            else:
                resp = {"plan": None, "error": p.stderr.strip() or f"Process exited with code {p.returncode}"}
        except Exception as e:
            resp = {"plan": None, "error": str(e)}

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))

    def handle_execute(self, req_data):
        sql = req_data.get("sql", "").strip()
        dataset = req_data.get("dataset", "100k")
        options = req_data.get("options", {})

        if not sql:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(json.dumps({"error": "Empty SQL query"}).encode("utf-8"))
            return

        # Parse AST eagerly so it is immediately available in the UI
        ast_info = self.parse_ast_info(sql)

        table_args = self.build_engine_table_args(sql, dataset)

        # 1. Execute Query Engine (C++17) with Telemetry Flags
        cmd = [self.engine_bin] + table_args + ["--sql", sql, "--sort-stats", "--join-stats"]

        mem_mb = options.get("memory_limit_mb", 512)
        if mem_mb and mem_mb != 512:
            cmd.extend(["--memory-limit-mb", str(mem_mb)])
        if options.get("no_pushdown", False):
            cmd.append("--no-pushdown")
        if options.get("no_join_swap", False):
            cmd.append("--no-join-swap")

        eng_header = []
        eng_rows = []
        eng_error = None
        telemetry = {"sort": None, "join": None}

        t0 = time.perf_counter()
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
            t1 = time.perf_counter()
            eng_ms = (t1 - t0) * 1000.0

            if p.returncode != 0:
                eng_error = p.stderr.strip() or f"Engine exited with status {p.returncode}"
            else:
                reader = csv.reader(io.StringIO(p.stdout))
                all_r = list(reader)
                if all_r:
                    eng_header = all_r[0]
                    num_cols = len(eng_header)
                    for r in all_r[1:]:
                        if len(r) == 0 and num_cols == 1:
                            eng_rows.append([""])
                        else:
                            eng_rows.append(r)

                if p.stderr:
                    for line in p.stderr.split("\n"):
                        line = line.strip()
                        if line.startswith("sort:"):
                            telemetry["sort"] = line
                        elif line.startswith("join:"):
                            telemetry["join"] = line
        except subprocess.TimeoutExpired:
            t1 = time.perf_counter()
            eng_ms = (t1 - t0) * 1000.0
            eng_error = "Engine execution timed out after 30 seconds"
        except Exception as e:
            t1 = time.perf_counter()
            eng_ms = (t1 - t0) * 1000.0
            eng_error = str(e)

        # 2. Execute SQLite Oracle
        sq_header = []
        raw_sq = []
        sq_rows = []
        sq_error = None
        sq_ms = 0.0

        try:
            conn = self.db_manager.get_connection(dataset)
            cur = conn.cursor()
            t0 = time.perf_counter()
            cur.execute(sql)
            raw_sq = cur.fetchall()
            t1 = time.perf_counter()
            sq_ms = (t1 - t0) * 1000.0

            sq_header = [d[0] for d in cur.description] if cur.description else []
            sq_rows = [[str(v) if v is not None else "" for v in row] for row in raw_sq]
        except Exception as e:
            sq_error = str(e)

        # 3. Correctness Verification & Matching
        matched = False
        diff_reason = None
        is_ordered = bool(re.search(r"\border\s+by\b", sql, re.IGNORECASE))

        if eng_error:
            matched = False
            diff_reason = f"Engine error: {eng_error}"
        elif sq_error:
            matched = False
            diff_reason = f"SQLite error: {sq_error}"
        elif len(eng_rows) != len(raw_sq):
            matched = False
            diff_reason = f"Row count differs: Engine returned {len(eng_rows)} rows, SQLite returned {len(raw_sq)} rows"
        elif len(eng_header) != len(sq_header):
            matched = False
            diff_reason = f"Column count differs: Engine returned {len(eng_header)} columns, SQLite returned {len(sq_header)} columns"
        else:
            e_compare = eng_rows
            s_compare = raw_sq
            if not is_ordered and len(eng_rows) > 0:
                e_compare = sorted(eng_rows, key=make_row_sort_key)
                s_compare = sorted(raw_sq, key=make_row_sort_key)

            mismatch_found = False
            for r_idx, (er, sr) in enumerate(zip(e_compare, s_compare)):
                if not rows_match(er, sr):
                    mismatch_found = True
                    for c_idx, (ec, sc) in enumerate(zip(er, sr)):
                        if not cells_match(ec, sc):
                            col_name = eng_header[c_idx] if c_idx < len(eng_header) else f"col_{c_idx}"
                            diff_reason = f"Value mismatch at row {r_idx + 1}, column '{col_name}': Engine='{ec}', SQLite='{sc}'"
                            break
                    break

            if not mismatch_found:
                matched = True
                diff_reason = None
            else:
                matched = False

        speedup = sq_ms / max(eng_ms, 0.001)

        resp = {
            "matched": matched,
            "diff_reason": diff_reason,
            "is_ordered": is_ordered,
            "speedup": round(speedup, 2),
            "telemetry": telemetry,
            "ast": ast_info,
            "engine": {
                "elapsed_ms": eng_ms,
                "header": eng_header,
                "rows": eng_rows,
                "error": eng_error
            },
            "sqlite": {
                "elapsed_ms": sq_ms,
                "header": sq_header,
                "rows": sq_rows,
                "error": sq_error
            }
        }

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))


def main():
    preferred_port = 8080
    if len(sys.argv) > 1:
        try:
            preferred_port = int(sys.argv[1])
        except ValueError:
            pass

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    SideBySideHandler.db_manager = DatabaseManager(repo_root)

    # Preload default 100k SQLite database in background thread for instant response
    threading.Thread(target=SideBySideHandler.db_manager.get_connection, args=("100k",), daemon=True).start()

    http.server.HTTPServer.allow_reuse_address = True
    server = None
    actual_port = preferred_port
    candidate_ports = [preferred_port] + [p for p in [8080, 8081, 8082, 8085, 8000, 8888] if p != preferred_port]
    for p in candidate_ports:
        try:
            server = http.server.HTTPServer(("0.0.0.0", p), SideBySideHandler)
            actual_port = p
            break
        except OSError:
            continue

    if not server:
        sys.stderr.write(f"Error: Could not bind to any candidate ports: {candidate_ports}\n")
        sys.exit(1)

    print(f"==================================================================")
    print(f"  Fast-SQL - Complete Query Engine Studio & Side-by-Side Runner")
    print(f"  Listening at: http://localhost:{actual_port}")
    print(f"  Press Ctrl+C to stop.")
    print(f"==================================================================")
    sys.stdout.flush()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server.")
        server.server_close()


if __name__ == "__main__":
    main()

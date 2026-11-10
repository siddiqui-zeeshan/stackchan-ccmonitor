FROM python:3.12-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY cc_monitor.py preview.html ./
EXPOSE 8810
CMD ["python", "cc_monitor.py"]

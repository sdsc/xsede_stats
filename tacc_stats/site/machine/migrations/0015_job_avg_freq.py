# Generated by Django 2.0.7 on 2018-10-05 16:37

from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('machine', '0014_job_avg_page_hitrate'),
    ]

    operations = [
        migrations.AddField(
            model_name='job',
            name='avg_freq',
            field=models.FloatField(null=True),
        ),
    ]
